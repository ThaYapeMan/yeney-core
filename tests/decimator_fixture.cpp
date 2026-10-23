// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decimator.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
    auto begin = std::chrono::steady_clock::now();
    while (input.read(reinterpret_cast<char *>(block), sizeof block) || input.gcount()) {
        size_t n = size_t(input.gcount()) / sizeof(yeney::Frame);
        count += n;
        fir.push(block, n);
        while ((n = fir.take(result, 4096)))
            output.write(reinterpret_cast<char *>(result), n * sizeof(yeney::Frame));
    }
    fir.end((count + fir.factor() - 1) / fir.factor());
    fir.pad();
    size_t n;
    while ((n = fir.take(result, 4096)))
        output.write(reinterpret_cast<char *>(result), n * sizeof(yeney::Frame));
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::cout << "rate=" << rate << " target=" << fir.outputRate() << " taps=" << fir.coefficients().size()
              << " frames=" << fir.produced() << " clips=" << fir.clips()
              << " speed=" << (double(count) / rate / seconds) << "x\n";
}
