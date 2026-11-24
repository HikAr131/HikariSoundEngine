// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "wind_fingerprint.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace hikari {
using namespace wind;
namespace { constexpr double kPi = 3.141592653589793238462643383279502884; }

double WindFingerprint::windowValue(std::size_t index) noexcept {
    const double u = static_cast<double>(std::min(index, kTaps - 1 - index)) / static_cast<double>(kTaps - 1);
    if (u >= kTukeyAlpha / 2) return 1;
    return 0.5 * (1 + std::cos(kPi * (2 * u / kTukeyAlpha - 1)));
}

double WindFingerprint::bandEdge(std::size_t band) noexcept {
    return kBandLowHz * std::pow(kBandSpan, static_cast<double>(band) / static_cast<double>(kBands));
}

WindFingerprint::WindFingerprint() {
    for (std::size_t index = 0; index < kTaps; ++index) window_[index] = windowValue(index);
    const double binHz = kRate / static_cast<double>(kDecimation) / static_cast<double>(kFingerprintFft);
    for (std::size_t band = 0; band < kBands; ++band) {
        const double low = bandEdge(band), high = bandEdge(band + 1);
        for (std::size_t bin = 0; bin < kBins; ++bin) {
            const double center = static_cast<double>(bin);
            weights_[band][bin] = std::max(0.0, std::min((center + 0.5) * binHz, high) - std::max((center - 0.5) * binHz, low)) / binHz;
        }
    }
}

WindFingerprint::Energies WindFingerprint::analyze(const float* stereo) const {
    std::array<double, kBins> power{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        std::array<std::complex<double>, kFingerprintFft> data{};
        for (std::size_t index = 0; index < kTaps; ++index) {
            double sum = 0;
            for (std::size_t step = 0; step < kDecimation; ++step) sum += stereo[(index * kDecimation + step) * 2 + channel];
            data[index] = sum / static_cast<double>(kDecimation) * window_[index];
        }
        fft_.transform(data.data());
        for (std::size_t bin = 0; bin < kBins; ++bin) power[bin] += std::norm(data[bin]);
    }
    Energies energies{};
    for (std::size_t band = 0; band < kBands; ++band) {
        double sum = 0;
        for (std::size_t bin = 0; bin < kBins; ++bin) sum += weights_[band][bin] * power[bin];
        energies[band] = static_cast<float>(std::log(std::max(kPowerFloor, sum)));
    }
    return energies;
}

std::array<double, kBands> windProfile(const std::array<double, kBands>& meanPower) noexcept {
    std::array<double, kBands> profile{};
    double sum = 0;
    for (std::size_t band = 0; band < kBands; ++band) {
        profile[band] = std::sqrt(std::sqrt(std::max(0.0, meanPower[band])));
        sum += profile[band] * profile[band];
    }
    if (sum > 0) for (auto& value : profile) value /= std::sqrt(sum);
    return profile;
}
} // namespace hikari
