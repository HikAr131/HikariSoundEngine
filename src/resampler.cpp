// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "resampler.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kBeta = 8.6;
constexpr double kPassband = 0.92;
constexpr int kHalfTaps = 24;
double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 96; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}
}

RationalResampler::RationalResampler(unsigned inputRate, unsigned outputRate) {
    if (inputRate < 8000 || outputRate < 8000 || inputRate > 384000 || outputRate > 384000)
        throw std::invalid_argument("Unsupported resampling rate");
    const unsigned divisor = std::gcd(inputRate, outputRate);
    up_ = outputRate / divisor;
    down_ = inputRate / divisor;
    if (identity()) { half_ = 1; table_ = {1.0f, 0.0f}; return; }
    if (up_ > 2048) throw std::invalid_argument("Unsupported resampling ratio");
    const double cutoff = kPassband * std::min(1.0, static_cast<double>(outputRate) / inputRate);
    half_ = static_cast<std::int64_t>(std::ceil(kHalfTaps / std::min(1.0, static_cast<double>(outputRate) / inputRate)));
    const std::size_t count = taps();
    table_.resize(static_cast<std::size_t>(up_) * count);
    const double scale = besselI0(kBeta);
    std::vector<double> taps(count);
    for (std::int64_t phase = 0; phase < up_; ++phase) {
        const double fraction = static_cast<double>(phase) / static_cast<double>(up_);
        double sum = 0;
        for (std::size_t tap = 0; tap < count; ++tap) {
            const double offset = static_cast<double>(tap) - static_cast<double>(half_ - 1) - fraction;
            const double ratio = offset / static_cast<double>(half_);
            const double argument = kPi * cutoff * offset;
            const double sinc = std::abs(argument) < 1e-12 ? 1 : std::sin(argument) / argument;
            taps[tap] = cutoff * sinc * besselI0(kBeta * std::sqrt(std::max(0.0, 1 - ratio * ratio))) / scale;
            sum += taps[tap];
        }
        for (std::size_t tap = 0; tap < count; ++tap)
            table_[static_cast<std::size_t>(phase) * count + tap] = static_cast<float>(taps[tap] / sum);
    }
}
} // namespace hikari
