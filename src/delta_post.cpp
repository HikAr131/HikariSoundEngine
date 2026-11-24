// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "delta_post.h"

namespace hikari {
void DeltaPost::prepare(double rate, unsigned channels) {
    channels_ = channels;
    footstep_.prepare(rate, channels);
    boom_.prepare(rate, channels);
    limiter_.prepare(rate, channels);
}

void DeltaPost::reset() noexcept {
    footstep_.reset();
    boom_.reset();
    limiter_.reset();
}

void DeltaPost::apply(const DeltaProfile& profile) noexcept {
    footstep_.setGainDb(footstepGainDb(profile.footstep));
    boom_.setLevel(profile.boom);
}

void DeltaPost::process(float* interleaved, std::size_t frames) noexcept {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float* sample = interleaved + frame * channels_;
        footstep_.processFrame(sample);
        boom_.processFrame(sample);
        limiter_.processFrame(sample);
    }
}
} // namespace hikari
