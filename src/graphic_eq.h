// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "parameters.h"

#include <array>
#include <complex>
#include <cstddef>
#include <vector>

namespace hikari {

class GraphicEq {
public:
    static constexpr std::size_t kPartitionFrames = 256;
    static constexpr std::size_t kFilterTaps = 16384;
    void configure(const std::vector<GraphicBand>& points, double sampleRate, unsigned channels);
    void process(float* interleaved, std::size_t frames) noexcept;
    static double targetDb(const std::vector<GraphicBand>& points, double frequency);
    double responseDb(double frequency) const;
    unsigned latencyFrames() const noexcept { return spectra_.empty() ? 0 : static_cast<unsigned>(kPartitionFrames); }
    const std::vector<double>& impulseResponse() const noexcept { return impulse_; }

private:
    static constexpr std::size_t kTransformSize = 2 * kPartitionFrames;
    static constexpr std::size_t kSpectrumSize = kPartitionFrames + 1;
    using Complex = std::complex<double>;
    struct Channel {
        std::vector<Complex> history;
        std::array<double, kPartitionFrames> input{}, output{}, overlap{};
        std::array<Complex, kTransformSize> work{}, accumulator{};
    };
    void transformBlock(std::array<Complex, kTransformSize>& block, bool inverse) const noexcept;
    void completeBlock() noexcept;
    std::array<Complex, kTransformSize / 2> twiddles_{};
    std::array<std::size_t, kTransformSize> reverse_{};
    std::vector<Complex> spectra_;
    std::vector<Channel> channels_;
    std::vector<double> impulse_;
    std::size_t partitions_ = 0, head_ = 0, position_ = 0;
    double sampleRate_ = 0, scalar_ = 1;
};

void runGraphicTests();

} // namespace hikari
