// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "sliding_window.h"
#include <array>
#include <cstddef>
#include <vector>

namespace hikari {
// Peak limiter with linked channels. The gain is the lookahead minimum of the required gain, released by
// one pole and averaged over the lookahead, so it never exceeds what any frame inside the window needs.
// With true peak detection the required gain also covers the waveform between frames.
class LookaheadLimiter {
public:
    static constexpr std::size_t kTruePeakHalf = 12;
    static constexpr std::size_t kTruePeakPhases = 8;
    // Room for what the short interpolator misses and for the moving gain itself.
    static constexpr double kTruePeakMarginDb = 0.4;
    void prepare(double rate, unsigned channels, double lookaheadSeconds, double ceiling,
                 double releaseSeconds, bool truePeak);
    void reset() noexcept;
    std::size_t latencyFrames() const noexcept { return lookahead_; }
    // One interleaved frame in place; returns the gain applied to the frame that leaves.
    float processFrame(float* frame) noexcept;
private:
    float intervalPeak() const noexcept;
    unsigned channels_ = 0;
    std::size_t frames_ = 0, lookahead_ = 0, window_ = 1, position_ = 0, averagePosition_ = 0;
    float ceiling_ = 1, target_ = 1, releaseStep_ = 0, released_ = 1, previousInterval_ = 0;
    double averageSum_ = 0;
    bool truePeak_ = false;
    SlidingMax deepest_, around_;
    std::vector<float> delay_, peaks_, average_;
    std::array<std::array<float, kTruePeakHalf * 2>, kTruePeakPhases - 1> phases_{};
};
} // namespace hikari
