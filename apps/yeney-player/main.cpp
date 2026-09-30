// Copyright (c) 2026 Jaap van Vliet
// Original implementation for the YeneY project.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Licensed under the PolyForm Noncommercial License 1.0.0. See LICENSE.
// THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

#include "core/player.h"
#include "sinks.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
std::atomic_bool stopped{false};
static_assert(std::atomic_bool::is_always_lock_free, "signal flag must be lock free");
extern "C" void stopPlayer(int) { stopped.store(true); }
int main(int argc, char **argv) {
    try {
        yeney::Config cfg;
        std::string sinkName = "null";
        int level = 1;
        uint32_t maxRate = 48000;
        bool haveName = false, haveMac = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "yeney-player -n <name> -m <mac> [-s <host>[:port]] [--sink null|wav:<path>] "
                             "[--max-rate <Hz>] [-d <level>]\n";
                return 0;
            }
            if (i + 1 == argc)
                throw std::invalid_argument("missing value for " + arg);
            std::string v = argv[++i];
            if (arg == "-n") {
                cfg.name = v;
                haveName = true;
            } else if (arg == "-m") {
                haveMac = true;
                unsigned m[6];
                int end = 0;
                if (std::sscanf(v.c_str(), "%x:%x:%x:%x:%x:%x%n", m, m + 1, m + 2, m + 3, m + 4, m + 5,
                                &end) != 6 ||
                    size_t(end) != v.size())
                    throw std::invalid_argument("invalid MAC");
                for (unsigned j = 0; j < 6; ++j) {
                    if (m[j] > 255)
                        throw std::invalid_argument("invalid MAC");
                    cfg.mac[j] = m[j];
                }
            } else if (arg == "-s") {
                auto at = v.find(':');
                cfg.server = v.substr(0, at);
                if (at != std::string::npos) {
                    size_t consumed;
                    unsigned long port = std::stoul(v.substr(at + 1), &consumed);
                    if (!port || port > 65535 || consumed != v.size() - at - 1)
                        throw std::invalid_argument("invalid port");
                    cfg.port = port;
                }
            } else if (arg == "--sink")
                sinkName = v;
            else if (arg == "--max-rate") {
                const char *message = "--max-rate must be an integer in 44100..384000 Hz";
                if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument(message);
                unsigned long rate;
                try {
                    rate = std::stoul(v);
                } catch (const std::exception &) {
                    throw std::invalid_argument(message);
                }
                if (rate < 44100 || rate > 384000)
                    throw std::invalid_argument(message);
                maxRate = rate;
            } else if (arg == "-d") {
                level = std::stoi(v);
                if (level < 0 || level > 5)
                    throw std::invalid_argument("debug level 0..5");
            } else
                throw std::invalid_argument("unknown option: " + arg);
        }
        if (!haveName || !haveMac || cfg.name.empty())
            throw std::invalid_argument("-n <name> and -m <mac> are required");
        std::unique_ptr<yeney::Sink> sink;
        if (sinkName == "null")
            sink = std::make_unique<yeney::NullSink>(maxRate);
        else if (sinkName.rfind("wav:", 0) == 0 && !sinkName.substr(4).empty())
            sink = std::make_unique<yeney::WavSink>(sinkName.substr(4), maxRate);
        else
            throw std::invalid_argument("unknown sink");
        cfg.log = [level](const std::string &s) {
            if (level)
                std::cout << s << std::endl;
        };
        std::signal(SIGINT, stopPlayer);
        std::signal(SIGTERM, stopPlayer);
        yeney::Player player(std::move(cfg), *sink);
        player.run(stopped);
        if (auto wav = dynamic_cast<yeney::WavSink *>(sink.get()))
            wav->close();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "yeney-player: " << e.what() << '\n';
        return 1;
    }
}
