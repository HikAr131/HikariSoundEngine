// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "boom_compressor.h"
#include <algorithm>
#include <cmath>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
double step(double seconds, double rate) { return 1 - std::exp(-1.0 / (seconds * rate)); }
double smooth(double position) { return 0.5 - 0.5 * std::cos(kPi * position); }
bool sameCurve(const BoomCompressor::Curve& a, const BoomCompressor::Curve& b) {
    return a.thresholdDb == b.thresholdDb && a.ratio == b.ratio && a.kneeDb == b.kneeDb;
}
}

BoomCompressor::Curve BoomCompressor::curve(BoomLevel level) noexcept {
    switch (level) {
    case BoomLevel::medium: return {-10.8, 10, 1};
    case BoomLevel::strong: return {-20, 4, 6};
    case BoomLevel::off:
    case BoomLevel::light: break;
    }
    return {-10, 4, 6};
}

double BoomCompressor::reductionDb(const Curve& curve, double levelDb) noexcept {
    const double over = levelDb - curve.thresholdDb, slope = 1 - 1 / curve.ratio;
    if (2 * over <= -curve.kneeDb) return 0;
    if (2 * over >= curve.kneeDb) return slope * over;
    return slope * (over + curve.kneeDb / 2) * (over + curve.kneeDb / 2) / (2 * curve.kneeDb);
}

void BoomCompressor::prepare(double rate, unsigned channels) {
    channels_ = channels;
    delayFrames_ = static_cast<std::size_t>(std::lround(kLookaheadSeconds * rate));
    delay_.assign(delayFrames_ * channels, 0.0f);
    hold_.prepare(delayFrames_ + static_cast<std::size_t>(std::lround(kHoldSeconds * rate)));
    modeFrames_ = static_cast<std::size_t>(std::lround(kModeHoldSeconds * rate));
    attackStep_ = step(kAttackSeconds, rate);
    fastStep_ = step(kFastReleaseSeconds, rate);
    slowStep_ = step(kSlowReleaseSeconds, rate);
    switchStep_ = 1.0 / (kSwitchSeconds * rate);
    current_ = previous_ = curve(BoomLevel::light);
    reset();
}

void BoomCompressor::reset() noexcept {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    hold_.reset();
    position_ = modeCount_ = 0;
    reduction_ = 0;
    slow_ = false;
    amount_ = active_ ? 1 : 0;
    blend_ = 1;
    previous_ = current_;
}

void BoomCompressor::setLevel(BoomLevel level) noexcept {
    active_ = level != BoomLevel::off;
    if (!active_) return;
    const auto next = curve(level);
    if (sameCurve(next, current_)) return;
    // Nothing is heard of the old curve while the stage is off, so only a running one is blended.
    previous_ = amount_ > 0 ? current_ : next;
    current_ = next;
    blend_ = amount_ > 0 ? 0 : 1;
}

void BoomCompressor::processFrame(float* frame) noexcept {
    float newest = 0;
    float* slot = delay_.data() + position_ * channels_;
    for (unsigned channel = 0; channel < channels_; ++channel) {
        const float sample = std::isfinite(frame[channel]) ? frame[channel] : 0.0f;
        newest = std::max(newest, std::abs(sample));
        frame[channel] = slot[channel];
        slot[channel] = sample;
    }
    position_ = (position_ + 1) * channels_ == delay_.size() ? 0 : position_ + 1;
    const double levelDb = 20 * std::log10(std::max(1e-9, static_cast<double>(hold_.push(newest))));
    blend_ = std::min(1.0, blend_ + switchStep_);
    double wanted = reductionDb(current_, levelDb);
    if (blend_ < 1) {
        const double before = reductionDb(previous_, levelDb);
        wanted = before + smooth(blend_) * (wanted - before);
    }
    reduction_ += (wanted - reduction_) * (wanted > reduction_ ? attackStep_ : slow_ ? slowStep_ : fastStep_);
    // Two-state release: deep for long enough slows it, shallow for long enough makes it quick again.
    const bool toward = slow_ ? reduction_ <= kFastBelowDb : reduction_ >= kSlowAboveDb;
    modeCount_ = toward ? modeCount_ + 1 : 0;
    if (modeCount_ >= modeFrames_) { slow_ = !slow_; modeCount_ = 0; }
    amount_ = std::clamp(amount_ + (active_ ? switchStep_ : -switchStep_), 0.0, 1.0);
    if (amount_ <= 0) return;
    const auto gain = static_cast<float>(std::pow(10.0, -smooth(amount_) * reduction_ / 20));
    for (unsigned channel = 0; channel < channels_; ++channel) frame[channel] *= gain;
}
} // namespace hikari
