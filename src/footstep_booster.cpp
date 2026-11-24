// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "footstep_booster.h"
#include <algorithm>
#include <cmath>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kLimiterReleaseSeconds = 0.020;
double step(double seconds, double rate) { return 1 - std::exp(-1.0 / (seconds * rate)); }
// Raised cosine of a linear 0-1 position.
double smooth(double position) { return 0.5 - 0.5 * std::cos(kPi * position); }
}

void FootstepBooster::prepare(double rate, unsigned channels) {
    channels_ = channels;
    limiter_.prepare(rate, channels, kLookaheadSeconds, kCeiling, kLimiterReleaseSeconds, false);
    block_.prepare(static_cast<std::size_t>(std::lround(kBlockSeconds * rate)));
    dry_.assign(limiter_.latencyFrames() * channels, 0.0f);
    // One pole whose -3 dB point is exactly the requested cutoff, even this far below the sample rate.
    const double cosine = std::cos(2 * kPi * kLevelCutoffHz / rate);
    levelStep_ = 1 - (2 - cosine - std::sqrt(cosine * cosine - 4 * cosine + 3));
    attackStep_ = step(kAttackSeconds, rate);
    releaseStep_ = step(kReleaseSeconds, rate);
    switchStep_ = 1.0 / (kSwitchSeconds * rate);
    loudPeak_ = static_cast<float>(std::pow(10.0, kLoudPeakDb / 20));
    const double omega = 2 * kPi * kHighPassHz / rate;
    const double c = std::cos(omega), alpha = std::sin(omega) / (2 * kHighPassQ), a0 = 1 + alpha;
    b0_ = b2_ = (1 + c) / 2 / a0;
    b1_ = -(1 + c) / a0;
    a1_ = -2 * c / a0;
    a2_ = (1 - alpha) / a0;
    reset();
}

void FootstepBooster::reset() noexcept {
    limiter_.reset();
    block_.reset();
    std::fill(dry_.begin(), dry_.end(), 0.0f);
    dryPosition_ = 0;
    levels_ = {};
    highPass_ = {};
    gain_ = 1;
    mix_ = active_ ? 1 : 0;
}

void FootstepBooster::setGainDb(double db) noexcept {
    active_ = db > 0;
    if (active_) target_ = std::pow(10.0, db / 20);
}

void FootstepBooster::processFrame(float* frame) noexcept {
    float newest = 0;
    double loudest = 0;
    for (unsigned channel = 0; channel < channels_; ++channel) {
        const double sample = std::isfinite(frame[channel]) ? frame[channel] : 0;
        frame[channel] = static_cast<float>(sample);
        newest = std::max(newest, std::abs(frame[channel]));
        levels_[channel] += (sample * sample - levels_[channel]) * levelStep_;
        loudest = std::max(loudest, levels_[channel]);
    }
    const bool loud = block_.push(newest) >= loudPeak_;
    double wanted = 1;
    if (!loud) {
        const double rms = std::sqrt(std::max(0.0, loudest));
        wanted = target_ * rms > kLevelTarget ? std::min(target_, std::max(kLevelTarget / rms, kMinimumBoost)) : target_;
    }
    gain_ += (wanted - gain_) * (wanted < gain_ ? attackStep_ : releaseStep_);

    float wet[8], delayed[8];
    float* slot = dry_.data() + dryPosition_ * channels_;
    for (unsigned channel = 0; channel < channels_; ++channel) {
        delayed[channel] = slot[channel];
        slot[channel] = frame[channel];
        wet[channel] = static_cast<float>(frame[channel] * gain_);
    }
    dryPosition_ = (dryPosition_ + 1) * channels_ == dry_.size() ? 0 : dryPosition_ + 1;
    limiter_.processFrame(wet);
    mix_ = std::clamp(mix_ + (active_ ? switchStep_ : -switchStep_), 0.0, 1.0);
    const double wetGain = smooth(mix_);
    for (unsigned channel = 0; channel < channels_; ++channel) {
        auto& memory = highPass_[channel];
        const double x = wet[channel];
        double y = b0_ * x + memory.z1;
        memory.z1 = b1_ * x - a1_ * y + memory.z2;
        memory.z2 = b2_ * x - a2_ * y;
        if (std::abs(memory.z1) < 1e-30) memory.z1 = 0;
        if (std::abs(memory.z2) < 1e-30) memory.z2 = 0;
        frame[channel] = static_cast<float>(mix_ <= 0 ? delayed[channel] : delayed[channel] + wetGain * (y - delayed[channel]));
    }
}
} // namespace hikari
