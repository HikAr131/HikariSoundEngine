// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "output_limiter.h"
#include <algorithm>
#include <cmath>

namespace hikari {
void OutputLimiter::prepare(double rate, unsigned channels) {
    limiter_.prepare(rate, channels, kLookaheadSeconds, std::pow(10.0, kCeilingDb / 20), kReleaseSeconds, true);
    heavyGain_ = static_cast<float>(std::pow(10.0, -kHeavyDb / 20));
    history_.assign(static_cast<std::size_t>(std::lround(kWindowSeconds * rate)), 0);
    reset();
}

void OutputLimiter::reset() noexcept {
    limiter_.reset();
    std::fill(history_.begin(), history_.end(), static_cast<unsigned char>(0));
    position_ = heavy_ = 0;
    warning_.store(false, std::memory_order_relaxed);
}

void OutputLimiter::processFrame(float* frame) noexcept {
    const unsigned char heavy = limiter_.processFrame(frame) < heavyGain_ ? 1 : 0;
    heavy_ += heavy;
    heavy_ -= history_[position_];
    history_[position_] = heavy;
    position_ = position_ + 1 == history_.size() ? 0 : position_ + 1;
    const double share = static_cast<double>(heavy_) / static_cast<double>(history_.size());
    if (warning_.load(std::memory_order_relaxed) ? share < kClearBelow : share > kWarnAbove)
        warning_.store(!warning_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
} // namespace hikari
