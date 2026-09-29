// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "player.h"
#include "ring.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <deque>
#include <limits>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <thread>

namespace yeney {
namespace {
struct Socket {
    int fd=-1;
    ~Socket() { reset(); }
    void reset(int next=-1) { if(fd>=0) ::close(fd); fd=next; }
};
bool writable(int fd) { pollfd p{fd,POLLOUT,0}; return ::poll(&p,1,0)>0; }
void nonblock(int fd) {
    if(::fcntl(fd,F_SETFL,::fcntl(fd,F_GETFL,0)|O_NONBLOCK)<0) throw std::runtime_error("fcntl");
}
sockaddr_in address(const std::string& host,uint16_t port) {
    sockaddr_in numeric{}; numeric.sin_family=AF_INET; numeric.sin_port=htons(port);
    if(::inet_pton(AF_INET,host.c_str(),&numeric.sin_addr)==1) return numeric;
    addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    addrinfo* result=nullptr;
    if(::getaddrinfo(host.c_str(),nullptr,&hints,&result)) throw std::runtime_error("cannot resolve server: "+host);
    sockaddr_in addr=*reinterpret_cast<sockaddr_in*>(result->ai_addr); ::freeaddrinfo(result); addr.sin_port=htons(port); return addr;
}
void connectSocket(Socket& sock,const sockaddr_in& addr) {
    sock.reset(::socket(AF_INET,SOCK_STREAM,0));
    if(sock.fd<0) throw std::runtime_error("socket");
    nonblock(sock.fd);
    int yes=1; ::setsockopt(sock.fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
    if(::connect(sock.fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))<0&&errno!=EINPROGRESS)
        throw std::runtime_error("connect");
}
bool connected(int fd) {
    int err=0; socklen_t n=sizeof(err);
    return ::getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&n)==0&&!err;
}
bool sendBytes(int fd,Bytes& bytes) {
    if(bytes.empty()) return true;
    ssize_t n=::send(fd,bytes.data(),bytes.size(),MSG_NOSIGNAL);
    if(n>0) bytes.erase(bytes.begin(),bytes.begin()+n);
    else if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR) return false;
    return true;
}
}
struct Player::Impl {
    Config cfg; Sink& sink;
    Socket control,http,discovery;
    sockaddr_in server{};
    struct Resolution {
        std::atomic_bool ready{false};
        sockaddr_in addr{};
        std::string error;
    };
    std::shared_ptr<Resolution> resolution;
    ServerParser parser;
    Bytes outbound,request;
    Ring<uint8_t> input;
    struct Track {
        Format format;
        std::deque<Frame> output;
        size_t threshold=0, outputThreshold=0;
        int autostart=1;
        bool known=false, headerPcm=false, wave=false, ready=false, eof=false, decoded=false, boundary=false, gapless=false;
        uint64_t first=0, end=0, skipped=0;
    };
    std::deque<std::shared_ptr<Track>> output, boundaries;
    std::shared_ptr<Track> fetching,audible;
    size_t queued=0;
    uint64_t received=0, submitted=0, skip=0, bodyReceived=0;
    uint64_t contentLength=std::numeric_limits<uint64_t>::max();
    std::string headers;
    bool controlConnecting=false,httpConnecting=false,haveHeaders=false,everConnected=false;
    bool paused=false, playing=false, userPaused=false, ended=true, underrun=false, power=true;
    uint32_t wake=0,lastTick=0,lastStat=0,nextConnect=0,connectDeadline=0,httpDeadline=0,nextDiscovery=0;
    unsigned backoff=100;
    double credit=0;
    explicit Impl(Config c,Sink& s):cfg(std::move(c)),sink(s),input(cfg.streamBytes) {
        if(cfg.streamBytes<8||cfg.outputFrames<256||cfg.streamBytes>UINT32_MAX||cfg.outputFrames>UINT32_MAX/8)
            throw std::invalid_argument("buffer size outside supported range");
        submitted=sink.audibleFrames();
    }
    void log(const std::string& s) { if(cfg.log) cfg.log(s); }
    void send(const std::string& op,const Bytes& b) {
        auto bytes=packet(op,b); outbound.insert(outbound.end(),bytes.begin(),bytes.end()); log(op);
    }
    uint32_t elapsed() const {
        if(!audible) return 0;
        auto frames=sink.audibleFrames();
        return uint32_t((frames>=audible->first?frames-audible->first+audible->skipped:0)*1000/audible->format.rate);
    }
    void stat(const std::string& event,uint32_t stamp=0) {
        Status s; s.streamSize=input.capacity(); s.streamFull=input.size(); s.received=received;
        s.jiffies=jiffies(); s.outputSize=cfg.outputFrames*sizeof(Frame); s.outputFull=queued*sizeof(Frame); s.elapsed=elapsed();
        auto b=statusPacket(event,s,stamp); outbound.insert(outbound.end(),b.begin(),b.end());
        log(event+" jiffies="+std::to_string(s.jiffies)+" elapsed_ms="+std::to_string(s.elapsed));
    }
    void stopStream() { http.reset(); httpConnecting=false; input.clear(); request.clear(); }
    void clear(bool stop) {
        stopStream(); fetching.reset(); output.clear(); boundaries.clear(); queued=0; skip=0; credit=0;
        sink.flush();
        if(stop) { sink.stop(); audible.reset(); }
        submitted=sink.audibleFrames(); playing=false; paused=false; userPaused=false; wake=0; ended=true;
    }
    void flushStreaming() {
        stopStream();
        std::shared_ptr<Track> retained;
        for(auto it=output.begin();it!=output.end();) {
            auto t=*it;
            if(t->boundary) { t->eof=t->decoded=true; retained=t; ++it; }
            else { queued-=t->output.size(); it=output.erase(it); }
        }
        fetching=retained;
        if(!retained) {
            if(audible&&sink.audibleFrames()<submitted) fetching=audible;
            else { playing=false; ended=true; }
        }
        stat("STMf");
    }
    void disconnect() {
        control.reset(); controlConnecting=false; resolution.reset(); outbound.clear(); parser=ServerParser{};
        clear(true); nextConnect=jiffies()+backoff;
        log("reconnect backoff_ms="+std::to_string(backoff)); backoff=std::min(backoff*2,5000u);
    }
    void failStream(const std::string& why,uint8_t reason=1) {
        bool connectedStream=http.fd>=0;
        log("stream error: "+why); stopStream();
        if(fetching) { fetching->eof=true; fetching->decoded=true; }
        stat("STMn"); if(connectedStream) send("DSCO",Bytes{reason});
    }
    void discover(uint32_t now) {
        if(discovery.fd<0) {
            discovery.reset(::socket(AF_INET,SOCK_DGRAM,0));
            if(discovery.fd<0) return;
            nonblock(discovery.fd); int yes=1; ::setsockopt(discovery.fd,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes));
        }
        if(due(now,nextDiscovery)) {
            const uint8_t query[]={'e','N','A','M','E',0,'J','S','O','N',0,'U','U','I','D',0,'V','E','R','S',0};
            sockaddr_in to{}; to.sin_family=AF_INET; to.sin_port=htons(3483); ::inet_pton(AF_INET,cfg.discoveryAddress.c_str(),&to.sin_addr);
            ::sendto(discovery.fd,query,sizeof(query),0,reinterpret_cast<sockaddr*>(&to),sizeof(to));
            // Loopback also makes discovery usable in isolated localhost networks.
            if(cfg.discoveryAddress!="127.0.0.1") {
                to.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
                ::sendto(discovery.fd,query,sizeof(query),0,reinterpret_cast<sockaddr*>(&to),sizeof(to));
            }
            nextDiscovery=now+1000; log("discovery");
        }
        uint8_t b[2048]; sockaddr_in from{}; socklen_t n=sizeof(from);
        ssize_t got=::recvfrom(discovery.fd,b,sizeof(b),0,reinterpret_cast<sockaddr*>(&from),&n);
        if(got>0&&discoveryReply(b,got)) { cfg.server=inet_ntoa(from.sin_addr); discovery.reset(); log("discovered "+cfg.server); }
    }
    void controlStep(uint32_t now) {
        if(cfg.server.empty()) { discover(now); return; }
        if(control.fd<0) {
            if(!due(now,nextConnect)) return;
            try {
                sockaddr_in numeric{};
                if(::inet_pton(AF_INET,cfg.server.c_str(),&numeric.sin_addr)==1) {
                    server=address(cfg.server,cfg.port);
                } else {
                    if(!resolution) {
                        resolution=std::make_shared<Resolution>();
                        auto result=resolution; auto host=cfg.server; auto port=cfg.port;
                        // A detached resolver owns only its result. Neither player
                        // shutdown nor a server switch waits for system DNS timeouts.
                        std::thread([result,host,port] {
                            try { result->addr=address(host,port); }
                            catch(const std::exception& e) { result->error=e.what(); }
                            result->ready.store(true,std::memory_order_release);
                        }).detach();
                        return;
                    }
                    if(!resolution->ready.load(std::memory_order_acquire)) return;
                    if(!resolution->error.empty()) throw std::runtime_error(resolution->error);
                    server=resolution->addr; resolution.reset();
                }
                connectSocket(control,server); controlConnecting=true; connectDeadline=now+3000;
            }
            catch(const std::exception& e) { log(e.what()); disconnect(); }
            return;
        }
        if(controlConnecting) {
            if(due(now,connectDeadline)) { disconnect(); return; }
            if(!writable(control.fd)) return;
            if(!connected(control.fd)) { disconnect(); return; }
            controlConnecting=false;
            auto b=hello(cfg.mac,everConnected,received,sink.maxSampleRate()); outbound.insert(outbound.end(),b.begin(),b.end());
            log("HELO reconnect="+std::to_string(everConnected)); everConnected=true;
            // Reset backoff only after a stable connection, not a fast disconnect loop.
            connectDeadline=now+5000;
        } else if(due(now,connectDeadline)) backoff=100;
        if(!sendBytes(control.fd,outbound)) { disconnect(); return; }
        uint8_t b[8192]; ssize_t got=::recv(control.fd,b,sizeof(b),0);
        if(got==0||(got<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)) { disconnect(); return; }
        if(got>0) {
            try { for(auto& msg:parser.feed(b,got)) { command(msg); if(control.fd<0) break; } }
            catch(const std::exception& e) { log(e.what()); disconnect(); }
        }
    }
    void format(Track& t,const uint8_t* p) {
        if(p[0]!='p') throw std::invalid_argument("only PCM supported");
        t.headerPcm=p[1]=='?'||p[2]=='?'||p[3]=='?'||p[4]=='?';
        if(!t.headerPcm) { t.format=pcmFormat(p[1],p[2],p[3],p[4],sink.maxSampleRate()); t.known=true; }
    }
    void command(const Bytes& b) {
        const std::string op(b.begin(),b.begin()+4); size_t n=b.size();
        if(op=="strm") {
            if(n<28) throw std::runtime_error("short strm");
            uint32_t value=be32(b.data()+18);
            switch(b[4]) {
            case 't': stat("STMt",value); break;
            case 'q': clear(true); stat("STMf"); break;
            case 'f': flushStreaming(); break;
            case 'p':
                sink.pause(); paused=true; userPaused=!value; wake=value?jiffies()+value:0; credit=0;
                if(!value) stat("STMp");
                break;
            case 'u':
                userPaused=false; wake=value; paused=value&&!due(jiffies(),value);
                playing=true; credit=0;
                if(!paused) { wake=0; sink.resume(); }
                stat("STMr"); break;
            case 'a': skip+=uint64_t(value)*(audible?audible->format.rate:48000)/1000; break;
            case 's': start(b); break;
            default: log("unknown strm command");
            }
        } else if(op=="setd"&&n>=5&&b[4]==0) {
            if(n>5) { auto end=std::find(b.begin()+5,b.end(),0); cfg.name.assign(b.begin()+5,end); }
            Bytes name{0}; name.insert(name.end(),cfg.name.begin(),cfg.name.end()); name.push_back(0); send("SETD",name);
        } else if(op=="audg"&&n>=22) {
            sink.volume(b[12]?be32(b.data()+14):65536,b[12]?be32(b.data()+18):65536);
            log("audg");
        } else if(op=="aude"&&n>=6) { power=b[4]||b[5]; sink.power(power); log("aude"); }
        else if(op=="cont"&&n>=9&&fetching) {
            if(be32(b.data()+4)) { failStream("ICY metadata not supported for PCM"); return; }
            if(fetching->autostart>=2) fetching->autostart-=2;
        } else if(op=="codc"&&n>=9&&fetching) {
            try { format(*fetching,b.data()+4); } catch(const std::exception& e) { failStream(e.what()); }
        } else if(op=="serv"&&n>=8) {
            in_addr addr{}; std::memcpy(&addr.s_addr,b.data()+4,4); cfg.server=inet_ntoa(addr); cfg.port=3483;
            log("serv "+cfg.server); disconnect(); nextConnect=jiffies();
        }
    }
    void start(const Bytes& b) {
        bool gapless=fetching&&fetching->decoded&&!ended;
        if(fetching&&!fetching->decoded) clear(true);
        stopStream(); received=0; bodyReceived=0; contentLength=std::numeric_limits<uint64_t>::max();
        headers.clear(); haveHeaders=false; stat("STMf");
        fetching=std::make_shared<Track>(); auto& t=*fetching; t.gapless=gapless;
        t.autostart=b[5]-'0'; t.threshold=std::min(size_t(b[11])*1024,input.capacity());
        t.outputThreshold=b[16]; ended=false; underrun=false;
        output.push_back(fetching);
        // STMc must precede even decoder failure: LMS ignores status before it.
        stat("STMc");
        try {
            if(t.autostart<0||t.autostart>3) throw std::invalid_argument("autostart");
            if(b[6]!='?') format(t,b.data()+6);
            else if(t.autostart<2) throw std::invalid_argument("unknown codec requires cont/codc");
            if(b[15]&0x20) throw std::invalid_argument("TLS not advertised");
            sockaddr_in addr=server; addr.sin_port=htons(be16(b.data()+22));
            if(be32(b.data()+24)) std::memcpy(&addr.sin_addr.s_addr,b.data()+24,4);
            request.assign(b.begin()+28,b.end());
            connectSocket(http,addr); httpConnecting=true; httpDeadline=jiffies()+5000;
        } catch(const std::exception& e) { failStream(e.what()); }
    }
    void eof() {
        http.reset(); httpConnecting=false;
        if(fetching) fetching->eof=true;
        send("DSCO",Bytes{0});
    }
    void httpStep(uint32_t now) {
        if(http.fd<0) return;
        if(httpConnecting) {
            if(due(now,httpDeadline)) { failStream("HTTP connect timeout",4); return; }
            if(!writable(http.fd)) return;
            if(!connected(http.fd)) { failStream("HTTP connect",3); return; }
            httpConnecting=false; stat("STMe");
        }
        if(!sendBytes(http.fd,request)) { failStream("HTTP request"); return; }
        if(!request.empty()) return;
        uint8_t b[8192]; size_t room=haveHeaders?input.free():std::min(input.capacity(),sizeof(b));
        if(!room) return;
        ssize_t n=::recv(http.fd,b,std::min(room,sizeof(b)),0);
        if(n==0) {
            if(!haveHeaders||(bodyReceived<contentLength&&contentLength!=std::numeric_limits<uint64_t>::max())) failStream("truncated HTTP response");
            else eof();
            return;
        }
        if(n<0) { if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR) failStream("HTTP read",2); return; }
        size_t offset=0;
        if(!haveHeaders) {
            // Consume bytewise so the remaining body never exceeds available ring space.
            while(offset<size_t(n)&&!haveHeaders) {
                headers.push_back(char(b[offset++]));
                if(headers.size()>65536) { failStream("HTTP headers too large"); return; }
                if(headers.size()>=4&&headers.compare(headers.size()-4,4,"\r\n\r\n")==0) haveHeaders=true;
            }
            if(!haveHeaders) return;
            stat("STMh"); send("RESP",Bytes(headers.begin(),headers.end()));
            std::string lower=headers; std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){ return std::tolower(c); });
            if(lower.rfind("http/1.",0)!=0||lower.size()<12||lower[9]!='2') { failStream("HTTP status"); return; }
            if(lower.find("transfer-encoding:")!=std::string::npos) { failStream("HTTP transfer encoding unsupported"); return; }
            auto pos=lower.find("\r\ncontent-length:");
            if(pos!=std::string::npos) {
                try { contentLength=std::stoull(lower.substr(pos+17)); } catch(...) { failStream("invalid Content-Length"); return; }
            }
        }
        size_t amount=std::min<uint64_t>(size_t(n)-offset,contentLength-bodyReceived);
        // Header reads are capped by ring capacity, leaving room for body bytes.
        if(amount>input.free()) { failStream("HTTP body exceeded input capacity"); return; }
        input.push(b+offset,amount); received+=amount; bodyReceived+=amount;
        if(bodyReceived==contentLength) eof();
    }
    bool containerHeader(Track& t) {
        if(input.size()<12) {
            if(t.eof) throw std::invalid_argument("truncated PCM container");
            return false;
        }
        auto tag=[&](size_t at,const char* id) {
            for(size_t i=0;i<4;++i) if(input.at(at+i)!=uint8_t(id[i])) return false;
            return true;
        };
        bool wave=tag(0,"RIFF")&&tag(8,"WAVE");
        bool aiff=tag(0,"FORM")&&(tag(8,"AIFF")||tag(8,"AIFC"));
        if(!wave&&!aiff) throw std::invalid_argument("PCM '?' requires WAV or AIFF");
        auto number=[&](size_t at,unsigned width) {
            uint32_t value=0;
            for(unsigned i=0;i<width;++i) value=(value<<8)|input.at(at+(wave?width-1-i:i));
            return value;
        };
        auto validate=[&](const Format& f) {
            if(f.channels<1||f.channels>2||!f.rate||f.rate>sink.maxSampleRate()||f.bits<8||f.bits>32||f.bits%8)
                throw std::invalid_argument("unsupported PCM container format");
        };
        size_t pos=12;
        bool found=false;
        Format f;
        while(input.size()>=pos+8) {
            uint32_t length=number(pos+4,4);
            if(wave&&tag(pos,"data")) {
                if(!found) throw std::invalid_argument("WAV data before fmt");
                input.discard(pos+8); t.format=f; t.known=true; t.headerPcm=false; t.wave=true;
                // Streaming WAV lengths can be sentinel values, so transport EOF
                // remains authoritative, as it does for raw PCM.
                return true;
            }
            if(aiff&&tag(pos,"SSND")) {
                if(!found||length<8) throw std::invalid_argument("invalid AIFF sound chunk");
                if(input.size()<pos+16) break;
                uint32_t offset=number(pos+8,4);
                if(offset>length-8||offset>input.capacity()-std::min(input.capacity(),pos+16))
                    throw std::invalid_argument("AIFF sound offset too large");
                if(input.size()<pos+16+offset) break;
                input.discard(pos+16+offset); t.format=f; t.known=true; t.headerPcm=false;
                return true;
            }
            if(length>input.capacity()-std::min(pos+8,input.capacity()))
                throw std::invalid_argument("PCM container header exceeds ring capacity");
            if(input.size()<pos+8+length+(length&1)) break;
            if(wave&&tag(pos,"fmt ")) {
                if(length<16||number(pos+8,2)!=1) throw std::invalid_argument("WAV must be integer PCM");
                f.channels=number(pos+10,2); f.rate=number(pos+12,4); f.bits=number(pos+22,2); f.bigEndian=false;
                if(number(pos+20,2)!=f.channels*f.bits/8) throw std::invalid_argument("WAV block alignment");
                validate(f); found=true;
            }
            if(aiff&&tag(pos,"COMM")) {
                if(length<18) throw std::invalid_argument("short AIFF common chunk");
                f.channels=number(pos+8,2); f.bits=number(pos+14,2); f.bigEndian=true;
                uint16_t exponent=number(pos+16,2);
                uint64_t mantissa=(uint64_t(number(pos+18,4))<<32)|number(pos+22,4);
                double rate=std::ldexp(double(mantissa),int(exponent&0x7fff)-16383-63);
                if(exponent&0x8000||!std::isfinite(rate)||rate<1||rate>sink.maxSampleRate()||rate!=std::floor(rate))
                    throw std::invalid_argument("unsupported AIFF sample rate");
                f.rate=uint32_t(rate);
                if(tag(8,"AIFC")) {
                    if(length<22||(!tag(pos+26,"NONE")&&!tag(pos+26,"sowt")))
                        throw std::invalid_argument("compressed AIFC unsupported");
                    f.bigEndian=!tag(pos+26,"sowt");
                }
                validate(f); found=true;
            }
            pos+=8+length+(length&1);
        }
        if(t.eof) throw std::invalid_argument("truncated PCM container header");
        if(input.free()==0) throw std::invalid_argument("PCM container header fills ring");
        return false;
    }
    void decode() {
        if(!fetching||fetching->decoded||!haveHeaders) return;
        auto& t=*fetching;
        if(t.autostart>=2) return;
        try {
            if(t.headerPcm&&!containerHeader(t)) return;
            if(!t.known) return;
            if(!t.ready) {
                if(input.size()<t.threshold&&!t.eof) return;
                t.ready=true;
            }
            unsigned bytes=t.format.bits/8*t.format.channels; uint8_t sample[8];
            while(input.size()>=bytes&&queued<cfg.outputFrames) {
                input.pop(sample,bytes);
                Frame f=decodeFrame(sample,t.format);
                // WAV 8-bit PCM is unsigned; raw Slimproto PCM is signed.
                if(t.wave&&t.format.bits==8) { f.left=int32_t(uint32_t(f.left)^0x80000000u); f.right=int32_t(uint32_t(f.right)^0x80000000u); }
                t.output.push_back(f); ++queued;
            }
            size_t target=std::min(cfg.outputFrames,size_t(t.outputThreshold)*t.format.rate/10);
            if(t.autostart==0&&t.ready&&(t.output.size()>=target||t.eof)) {
                stat("STMl"); t.autostart=-1;
            }
            if(t.autostart==1&&(t.output.size()>=target||t.eof)) {
                if(!userPaused&&!wake) { playing=true; paused=false; sink.resume(); }
                t.autostart=-1;
            }
            if(t.eof&&input.size()<bytes) {
                if(input.size()) throw std::invalid_argument("partial PCM frame at EOF");
                t.decoded=true; stat("STMd");
            }
        } catch(const std::exception& e) { failStream(e.what()); }
    }
    void audibleEvents() {
        uint64_t position=sink.audibleFrames();
        while(!boundaries.empty()&&position>boundaries.front()->first) {
            audible=boundaries.front(); boundaries.pop_front(); stat("STMs");
        }
    }
    void outputStep(uint32_t now) {
        uint32_t delta=now-lastTick; lastTick=now;
        if(paused&&wake&&due(now,wake)) { paused=false; wake=0; sink.resume(); credit=0; }
        if(!playing||paused||!power) { credit=0; audibleEvents(); return; }
        if(sink.paced()) credit=std::min(credit+double(delta)/1000,0.020);
        size_t iterations=0;
        while(!output.empty()&&++iterations<=16) {
            auto t=output.front();
            if(t->output.empty()) {
                if(t->decoded) { t->end=submitted; output.pop_front(); continue; }
                break;
            }
            if(!t->ready) break;
            size_t count=std::min<size_t>(t->output.size(),256);
            if(skip) {
                size_t n=std::min<uint64_t>(count,skip);
                for(size_t i=0;i<n;++i) t->output.pop_front();
                queued-=n; skip-=n; t->skipped+=n; continue;
            }
            if(sink.paced()) count=std::min(count,size_t(std::max(0.0,std::floor(credit*t->format.rate))));
            if(!count) break;
            if(!t->boundary) {
                t->first=submitted; t->boundary=true;
                sink.trackBoundary(submitted,t->format,t->gapless); boundaries.push_back(t);
                log("boundary frame="+std::to_string(submitted)+" rate="+std::to_string(t->format.rate)+" gapless="+std::to_string(t->gapless));
            }
            Frame frames[256]; for(size_t i=0;i<count;++i) frames[i]=t->output[i];
            size_t accepted=sink.write(frames,count);
            if(accepted>count) throw std::runtime_error("sink accepted more frames than offered");
            for(size_t i=0;i<accepted;++i) t->output.pop_front();
            queued-=accepted; submitted+=accepted;
            if(sink.paced()) credit-=double(accepted)/t->format.rate;
            audibleEvents();
            if(!accepted) break;
            underrun=false;
        }
        audibleEvents();
        if(queued==0&&fetching&&fetching->decoded&&output.empty()&&sink.audibleFrames()>=submitted&&!ended) {
            stat("STMu"); ended=true; playing=false; credit=0;
        } else if(queued==0&&fetching&&!fetching->decoded&&fetching->ready) {
            if(!underrun) stat("STMo");
            underrun=true; credit=0;
        }
        if(due(now,lastStat+1000)) { stat("STMt"); lastStat=now; }
    }
    void run(const std::atomic_bool& stop) {
        lastTick=lastStat=jiffies(); nextConnect=nextDiscovery=lastTick;
        while(!stop.load()) {
            uint32_t now=jiffies(); controlStep(now);
            if(control.fd>=0&&!controlConnecting) { httpStep(now); decode(); outputStep(now); }
            pollfd p{control.fd,POLLIN,0}; ::poll(&p,control.fd>=0?1:0,2);
        }
        clear(true); control.reset(); log("shutdown");
    }
};
Player::Player(Config c,Sink& s):impl_(std::make_unique<Impl>(std::move(c),s)) {}
Player::~Player()=default;
void Player::run(const std::atomic_bool& stop) { impl_->run(stop); }
}
