// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decimator.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <time.h>
uint64_t threadCpuNs() {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value))
        throw std::runtime_error("conversion thread CPU clock unavailable");
    return uint64_t(value.tv_sec) * 1000000000ull + uint64_t(value.tv_nsec);
}
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    const auto rate = std::stoul(argv[1]);
    yeney::Decimator fir(rate);
    {
        std::ofstream taps(std::string(argv[3]) + ".taps", std::ios::binary);
        auto &h = fir.coefficients();
        taps.write(reinterpret_cast<const char *>(h.data()), h.size() * sizeof(double));
    }
    std::ifstream input(argv[2], std::ios::binary);
    std::ofstream output(argv[3], std::ios::binary);
    yeney::Frame block[4096], result[4096];
    uint64_t count = 0;
    uint64_t conversionNs = 0;
    auto take = [&] {
        const auto cpuBegin = threadCpuNs();
        const auto n = fir.take(result, 4096);
        conversionNs += threadCpuNs() - cpuBegin;
        return n;
    };
    auto begin = std::chrono::steady_clock::now();
    while (input.read(reinterpret_cast<char *>(block), sizeof block) || input.gcount()) {
        size_t n = size_t(input.gcount()) / sizeof(yeney::Frame);
        count += n;
        const auto cpuBegin = threadCpuNs();
        fir.push(block, n);
        conversionNs += threadCpuNs() - cpuBegin;
        while ((n = take()))
            output.write(reinterpret_cast<char *>(result), n * sizeof(yeney::Frame));
    }
    const auto cpuBegin = threadCpuNs();
    fir.end((count + fir.factor() - 1) / fir.factor());
    fir.pad();
    conversionNs += threadCpuNs() - cpuBegin;
    size_t n;
    while ((n = take()))
        output.write(reinterpret_cast<char *>(result), n * sizeof(yeney::Frame));
    output.flush();
    const double cpuSeconds = double(conversionNs) / 1000000000.;
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::cout << "rate=" << rate << " target=" << fir.outputRate() << " taps=" << fir.coefficients().size()
              << " frames=" << fir.produced() << " clips=" << fir.clips()
              << " speed=" << (double(count) / rate / seconds) << "x"
              << " cpu_speed=" << (double(count) / rate / cpuSeconds) << "x"
              << " cpu_seconds=" << cpuSeconds << " wall_seconds=" << seconds << "\n";
}
