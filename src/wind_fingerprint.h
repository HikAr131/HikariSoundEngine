// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "fft.h"
#include "wind_constants.h"
#include <array>
#include <vector>

namespace hikari {
// Sixteen log band energies of one 20 ms stereo sub-frame. References and live input share this code.
class WindFingerprint {
public:
    using Energies = std::array<float, wind::kBands>;
    WindFingerprint();
    Energies analyze(const float* stereo) const;
    static double windowValue(std::size_t index) noexcept;
    static double bandEdge(std::size_t band) noexcept;
    double bandWeight(std::size_t band, std::size_t bin) const noexcept { return weights_[band][bin]; }
private:
    static constexpr std::size_t kBins = wind::kFingerprintFft / 2 + 1;
    Fft<double> fft_{wind::kFingerprintFft};
    std::array<double, wind::kTaps> window_{};
    std::array<std::array<double, kBins>, wind::kBands> weights_{};
};

// Fourth-root band spectrum with unit length; its dot product is the presence similarity.
std::array<double, wind::kBands> windProfile(const std::array<double, wind::kBands>& meanPower) noexcept;
} // namespace hikari
