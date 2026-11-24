// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "lookahead_limiter.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kWindowBeta = 5.0;
// Interpolated values stay below this multiple of the largest frame under the taps.
constexpr float kOvershootBound = 3.0f;

double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}
}

void LookaheadLimiter::prepare(double rate, unsigned channels, double lookaheadSeconds, double ceiling,
                               double releaseSeconds, bool truePeak) {
    if (!(rate > 0) || channels == 0 || channels > 8) throw std::invalid_argument("Unsupported limiter format");
    channels_ = channels;
    truePeak_ = truePeak;
    lookahead_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(lookaheadSeconds * rate)));
    if (truePeak_ && lookahead_ <= kTruePeakHalf) throw std::invalid_argument("Limiter lookahead is too short");
    // A true peak is known kTruePeakHalf frames late, which shortens the window left for the gain ramp.
    window_ = lookahead_ - (truePeak_ ? kTruePeakHalf : 0) + 1;
    frames_ = lookahead_ + 2 * kTruePeakHalf + 1;
    ceiling_ = static_cast<float>(ceiling);
    target_ = static_cast<float>(ceiling * (truePeak ? std::pow(10.0, -kTruePeakMarginDb / 20) : 1.0));
    releaseStep_ = static_cast<float>(1 - std::exp(-1.0 / (releaseSeconds * rate)));
    deepest_.prepare(window_);
    around_.prepare(2 * kTruePeakHalf + 1);
    delay_.assign(frames_ * channels_, 0.0f);
    peaks_.assign(frames_, 0.0f);
    average_.assign(window_, 1.0f);
    for (std::size_t phase = 0; phase + 1 < kTruePeakPhases; ++phase) {
        const double fraction = static_cast<double>(phase + 1) / static_cast<double>(kTruePeakPhases);
        std::array<double, kTruePeakHalf * 2> taps{};
        double sum = 0;
        for (std::size_t tap = 0; tap < taps.size(); ++tap) {
            const double offset = static_cast<double>(tap) - static_cast<double>(kTruePeakHalf - 1) - fraction;
            const double ratio = offset / static_cast<double>(kTruePeakHalf);
            const double sinc = std::abs(offset) < 1e-12 ? 1 : std::sin(kPi * offset) / (kPi * offset);
            taps[tap] = sinc * besselI0(kWindowBeta * std::sqrt(std::max(0.0, 1 - ratio * ratio))) / besselI0(kWindowBeta);
            sum += taps[tap];
        }
        for (std::size_t tap = 0; tap < taps.size(); ++tap) phases_[phase][tap] = static_cast<float>(taps[tap] / sum);
    }
    reset();
}

void LookaheadLimiter::reset() noexcept {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    std::fill(peaks_.begin(), peaks_.end(), 0.0f);
    std::fill(average_.begin(), average_.end(), 1.0f);
    deepest_.reset();
    around_.reset();
    averageSum_ = static_cast<double>(window_);
    averagePosition_ = position_ = 0;
    released_ = 1;
    previousInterval_ = 0;
}

// Largest interpolated magnitude between the frame kTruePeakHalf behind the newest one and the frame after it.
float LookaheadLimiter::intervalPeak() const noexcept {
    constexpr std::size_t taps = 2 * kTruePeakHalf;
    float peak = 0;
    for (unsigned channel = 0; channel < channels_; ++channel) {
        float history[taps];
        for (std::size_t back = 0; back < taps; ++back)
            history[taps - 1 - back] = delay_[((position_ + frames_ - back) % frames_) * channels_ + channel];
        for (const auto& phase : phases_) {
            float sum = 0;
            for (std::size_t tap = 0; tap < taps; ++tap) sum += phase[tap] * history[tap];
            peak = std::max(peak, std::abs(sum));
        }
    }
    return peak;
}

float LookaheadLimiter::processFrame(float* frame) noexcept {
    float newest = 0;
    for (unsigned channel = 0; channel < channels_; ++channel) {
        const float sample = std::isfinite(frame[channel]) ? frame[channel] : 0.0f;
        delay_[position_ * channels_ + channel] = sample;
        newest = std::max(newest, std::abs(sample));
    }
    peaks_[position_] = newest;
    float peak = newest;
    if (truePeak_) {
        const float interval = around_.push(newest) * kOvershootBound > target_ ? intervalPeak() : 0.0f;
        peak = std::max(peaks_[(position_ + frames_ - kTruePeakHalf) % frames_], std::max(previousInterval_, interval));
        previousInterval_ = interval;
    }
    const float required = peak > target_ ? target_ / peak : 1.0f;
    const float floor = 1.0f - deepest_.push(1.0f - required);
    released_ = std::min(floor, released_ + (1.0f - released_) * releaseStep_);
    averageSum_ += static_cast<double>(released_) - static_cast<double>(average_[averagePosition_]);
    average_[averagePosition_] = released_;
    averagePosition_ = averagePosition_ + 1 == window_ ? 0 : averagePosition_ + 1;
    const float gain = static_cast<float>(std::min(1.0, averageSum_ / static_cast<double>(window_)));
    const std::size_t out = (position_ + frames_ - lookahead_) % frames_;
    for (unsigned channel = 0; channel < channels_; ++channel)
        frame[channel] = std::clamp(delay_[out * channels_ + channel] * gain, -ceiling_, ceiling_);
    position_ = position_ + 1 == frames_ ? 0 : position_ + 1;
    return gain;
}
} // namespace hikari
