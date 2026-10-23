// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "decimator.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace yeney {
namespace {
double i0(double x) {
    double sum = 1, term = 1;
    for (unsigned k = 1; k != 100; ++k) {
        term *= (x * x / 4) / (double(k) * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}
int32_t quantize(double value) {
    // Retain 8 guard bits through ReplayGain/mixing; the existing output packer
    // rounds to 24 bits once. Saturation prevents intersample overflow/wrap.
    return int32_t(std::clamp(std::round(value), double(INT32_MIN), double(INT32_MAX)));
}
} // namespace
uint32_t Decimator::targetRate(uint32_t rate, uint32_t cap) {
    if (rate <= cap)
        return rate;
    if (cap == 48000) {
        if (rate == 88200 || rate == 176400)
            return 44100;
        if (rate == 96000 || rate == 192000)
            return 48000;
    }
    throw std::runtime_error("unsupported decimation rate=" + std::to_string(rate));
}
Decimator::Decimator(uint32_t rate, uint64_t phase, const std::vector<Frame> &history)
    : source(rate), target(targetRate(rate)), divisor(source / target),
      radius((target == 44100 ? 160 : 128) * divisor), center(int64_t(phase)) {
    if (divisor != 2 && divisor != 4)
        throw std::invalid_argument("decimator requires factor 2 or 4");
    const double pi = std::acos(-1.0), cutoff = (20000.0 + target / 2.0) / 2 / source;
    constexpr double beta = 18.5;
    const double windowScale = i0(beta);
    filter.resize(2 * radius + 1);
    double gain = 0;
    for (int n = -int(radius); n <= int(radius); ++n) {
        const double x = double(n) / radius;
        const double sinc = n ? std::sin(2 * pi * cutoff * n) / (pi * n) : 2 * cutoff;
        const double tap = sinc * i0(beta * std::sqrt(std::max(0.0, 1 - x * x))) / windowScale;
        filter[n + radius] = tap;
        gain += tap;
    }
    for (auto &tap : filter)
        tap /= gain;
    base = -int64_t(radius);
    samples.resize(radius, Stereo{0, 0});
    const size_t count = std::min(history.size(), size_t(radius));
    for (size_t i = 0; i < count; ++i) {
        const auto &f = history[history.size() - count + i];
        samples[radius - count + i] = {double(f.left), double(f.right)};
    }
}
void Decimator::push(const Frame *p, size_t n) {
    received += n;
    lookahead(p, n);
}
void Decimator::lookahead(const Frame *p, size_t n) {
    samples.reserve(samples.size() + n);
    for (size_t i = 0; i < n; ++i)
        samples.push_back({double(p[i].left), double(p[i].right)});
}
void Decimator::end(uint64_t frames) { budget = frames; }
void Decimator::pad() {
    if (budget == UINT64_MAX)
        throw std::logic_error("decimator pad before end");
    const auto end = center + int64_t((budget - emitted) * divisor) + radius;
    const auto available = base + int64_t(samples.size());
    if (end >= available)
        samples.resize(samples.size() + size_t(end - available + 1), Stereo{0, 0});
}
Frame Decimator::convolve(size_t at) {
    double l[4]{}, r[4]{};
    // Symmetry halves multiplies. Independent accumulators permit SIMD and
    // avoid one long dependency chain without reassociating individual sums.
    for (unsigned k = 0; k < radius; ++k) {
        const auto &a = samples[at - radius + k], &b = samples[at + radius - k];
        const double h = filter[k];
        l[k & 3] += (a.left + b.left) * h;
        r[k & 3] += (a.right + b.right) * h;
    }
    const auto &c = samples[at];
    const double left = ((l[0] + l[1]) + (l[2] + l[3])) + c.left * filter[radius];
    const double right = ((r[0] + r[1]) + (r[2] + r[3])) + c.right * filter[radius];
    constexpr double upper = 2147483520.0, lower = -2147483776.0;
    if (left >= upper || left <= lower || right >= upper || right <= lower)
        ++clipped;
    return {quantize(left), quantize(right)};
}
size_t Decimator::take(Frame *p, size_t n) {
    size_t count = 0;
    while (count < n && emitted < budget && center + radius < base + int64_t(samples.size())) {
        const auto frame = convolve(size_t(center - base));
        p[count++] = frame;
        center += divisor;
        ++emitted;
    }
    if (center - radius - base > 8192) {
        const auto discard = size_t(center - radius - base);
        samples.erase(samples.begin(), samples.begin() + std::min(discard, samples.size()));
        base += discard;
    }
    return count;
}
} // namespace yeney
