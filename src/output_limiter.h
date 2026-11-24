// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "lookahead_limiter.h"
#include <atomic>
#include <cstddef>
#include <vector>

namespace hikari {
// The last stage: -1 dBFS with true peak detection, always on while the profile is. It also reports when too
// large a share of the recent frames is being held down hard.
class OutputLimiter {
public:
    static constexpr double kCeilingDb = -1;
    static constexpr double kLookaheadSeconds = 0.001;
    static constexpr double kReleaseSeconds = 0.050;
    static constexpr double kHeavyDb = 3;
    static constexpr double kWindowSeconds = 0.5;
    static constexpr double kWarnAbove = 0.20;
    static constexpr double kClearBelow = 0.10;
    void prepare(double rate, unsigned channels);
    void reset() noexcept;
    std::size_t latencyFrames() const noexcept { return limiter_.latencyFrames(); }
    bool warning() const noexcept { return warning_.load(std::memory_order_relaxed); }
    void processFrame(float* frame) noexcept;
private:
    LookaheadLimiter limiter_;
    float heavyGain_ = 0;
    std::vector<unsigned char> history_;
    std::size_t position_ = 0, heavy_ = 0;
    std::atomic<bool> warning_{false};
};
} // namespace hikari
