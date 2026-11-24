// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "boom_compressor.h"
#include "delta_profile.h"
#include "footstep_booster.h"
#include "output_limiter.h"
#include <cstddef>

namespace hikari {
// Everything the profile does after the equalizer: footsteps, loud bursts, final limiter. The delay is constant
// while the profile is on, whatever the single stages are set to.
class DeltaPost {
public:
    void prepare(double rate, unsigned channels);
    void reset() noexcept;
    void apply(const DeltaProfile& profile) noexcept;
    void process(float* interleaved, std::size_t frames) noexcept;
    bool limiterWarning() const noexcept { return limiter_.warning(); }
    std::size_t footstepFrames() const noexcept { return footstep_.latencyFrames(); }
    std::size_t boomFrames() const noexcept { return boom_.latencyFrames(); }
    std::size_t limiterFrames() const noexcept { return limiter_.latencyFrames(); }
    std::size_t latencyFrames() const noexcept { return footstepFrames() + boomFrames() + limiterFrames(); }
private:
    unsigned channels_ = 0;
    FootstepBooster footstep_;
    BoomCompressor boom_;
    OutputLimiter limiter_;
};
} // namespace hikari
