// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "delta_profile.h"
#include "sliding_window.h"
#include <cstddef>
#include <vector>

namespace hikari {
// Feed-forward peak compressor with lookahead and linked channels. The release slows down while the reduction
// stays deep, so bursts are held down instead of pumping. Off fades the reduction to nothing.
class BoomCompressor {
public:
    struct Curve { double thresholdDb, ratio, kneeDb; };
    static constexpr double kLookaheadSeconds = 0.002;
    static constexpr double kHoldSeconds = 0.010;
    static constexpr double kAttackSeconds = 0.0001;
    static constexpr double kFastReleaseSeconds = 0.020;
    static constexpr double kSlowReleaseSeconds = 0.250;
    static constexpr double kSlowAboveDb = 3;
    static constexpr double kFastBelowDb = 1;
    static constexpr double kModeHoldSeconds = 0.120;
    static constexpr double kSwitchSeconds = 0.020;
    static Curve curve(BoomLevel level) noexcept;
    static double reductionDb(const Curve& curve, double levelDb) noexcept;
    void prepare(double rate, unsigned channels);
    void reset() noexcept;
    void setLevel(BoomLevel level) noexcept;
    std::size_t latencyFrames() const noexcept { return delayFrames_; }
    bool slowRelease() const noexcept { return slow_; }
    void processFrame(float* frame) noexcept;
private:
    unsigned channels_ = 0;
    std::size_t delayFrames_ = 0, position_ = 0, modeFrames_ = 0, modeCount_ = 0;
    double attackStep_ = 0, fastStep_ = 0, slowStep_ = 0, switchStep_ = 0;
    double reduction_ = 0, amount_ = 0, blend_ = 1;
    bool active_ = false, slow_ = false;
    Curve current_{}, previous_{};
    SlidingMax hold_;
    std::vector<float> delay_;
};
} // namespace hikari
