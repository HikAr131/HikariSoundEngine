// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "lookahead_limiter.h"
#include "sliding_window.h"
#include <array>
#include <cstddef>
#include <vector>

namespace hikari {
// Raises quiet passages by up to a fixed gain. Loud blocks are left alone, the boosted level is capped, and a
// short lookahead limiter plus a 60 Hz high-pass follow. The delay stays constant while the stage is off.
class FootstepBooster {
public:
    static constexpr double kLookaheadSeconds = 0.00075;
    static constexpr double kCeiling = 0.966;
    static constexpr double kLevelCutoffHz = 0.1;
    static constexpr double kLevelTarget = 0.32;
    static constexpr double kMinimumBoost = 1.06;
    static constexpr double kLoudPeakDb = -9;
    static constexpr double kBlockSeconds = 0.008;
    static constexpr double kAttackSeconds = 0.002;
    static constexpr double kReleaseSeconds = 0.020;
    static constexpr double kHighPassHz = 60;
    static constexpr double kHighPassQ = 0.707;
    static constexpr double kSwitchSeconds = 0.020;
    void prepare(double rate, unsigned channels);
    void reset() noexcept;
    void setGainDb(double db) noexcept;
    std::size_t latencyFrames() const noexcept { return limiter_.latencyFrames(); }
    void processFrame(float* frame) noexcept;
private:
    struct Memory { double z1 = 0, z2 = 0; };
    unsigned channels_ = 0;
    double levelStep_ = 0, attackStep_ = 0, releaseStep_ = 0, switchStep_ = 0;
    double target_ = 1, gain_ = 1, mix_ = 0;
    float loudPeak_ = 1;
    bool active_ = false;
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    std::array<double, 8> levels_{};
    std::array<Memory, 8> highPass_{};
    SlidingMax block_;
    LookaheadLimiter limiter_;
    std::vector<float> dry_;
    std::size_t dryPosition_ = 0;
};
} // namespace hikari
