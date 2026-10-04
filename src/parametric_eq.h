// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "parameters.h"
#include "graphic_eq.h"

#include <array>
#include <cstddef>
#include <vector>

namespace hikari {

class ParametricEq {
public:
    void configure(const Parameters& parameters, double sampleRate, unsigned channels);
    void process(float* interleaved, std::size_t frames) noexcept;
    // Input history needed before a fresh instance reproduces a continuously running one:
    // the full FIR plus one partition, and the 1e-9 decay span of the slowest biquad pole.
    std::size_t warmupFrames(std::size_t limit) const noexcept;
    static double responseDb(const Filter& filter, double frequency, double sampleRate = 48000);

private:
    struct Coefficients {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct ChannelState { double z1 = 0, z2 = 0; };
    struct Section {
        Coefficients coefficients;
        std::array<ChannelState, 8> channels{};
    };
    static Coefficients design(const Filter& filter, double sampleRate);
    std::vector<Section> sections_;
    double preamp_ = 1;
    unsigned channels_ = 0;
    bool bypass_ = true;
    GraphicEq graphic_;
};

void runEqTests();

} // namespace hikari
