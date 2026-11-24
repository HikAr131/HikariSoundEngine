// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "wind_canceller.h"
#include <algorithm>
#include <cmath>

namespace hikari {
using namespace wind;
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::size_t kCrossfadeFrames = static_cast<std::size_t>(kSwitchSeconds * kRate);
constexpr double kContinuedFadeSeconds = 0.05;
constexpr double kSameVoice = 1e-3;
constexpr std::int64_t kWarmFrames = 160;
double smooth(double position) noexcept { return 0.5 - 0.5 * std::cos(kPi * position); }
}

WindCanceller::WindCanceller(unsigned rate, unsigned channels)
    : rate_(rate), channels_(channels), blockFrames_(static_cast<std::size_t>(std::lround(rate * static_cast<double>(kBlock) / kRate))),
      toDevice_(static_cast<unsigned>(kRate), rate), model_(kModelRing * 2, 0.0f) {
    fadeInStep_ = 1 / (kFadeInSeconds * rate);
    fadeOutStep_ = 1 / (kFadeOutSeconds * rate);
    activeStep_ = 1 / (2 * kSwitchSeconds * rate);
    for (std::size_t band = 0; band < split_.size(); ++band) {
        const double omega = 2 * kPi * kCrossovers[band] / rate;
        const double c = std::cos(omega), alpha = std::sin(omega) / 1.41421356237, a0 = 1 + alpha;
        split_[band] = {(1 - c) / (2 * a0), (1 - c) / a0, (1 - c) / (2 * a0), -2 * c / a0, (1 - alpha) / a0};
    }
    restart();
}

void WindCanceller::restart() noexcept {
    inputSplit_ = {};
    modelSplit_ = {};
    inputEnergy_ = {};
    modelEnergy_ = {};
    cross_ = {};
    blockInput_ = {};
    blockModel_ = {};
    blockCross_ = {};
    energyPosition_ = blockPosition_ = 0;
    bandGain_.fill(1);
    bandTarget_.fill(1);
    level_ = levelStep_ = 0;
}

void WindCanceller::update(const WindReferenceSet* references, const WindLock& lock) noexcept {
    if (!references) return;
    interpolator_ = &references->interpolator();
    if (!lock.locked || lock.reference < 0 || static_cast<std::size_t>(lock.reference) >= references->items().size()) {
        if (locked_) fadeStep_ = lock.abrupt ? -1 / (kSwitchSeconds * rate_) : -fadeOutStep_;
        locked_ = false;
        return;
    }
    // Found before the newest break, so nobody knows where it is now.
    if (lock.breaks != breaks_) return;
    Voice next;
    next.reference = &references->items()[static_cast<std::size_t>(lock.reference)];
    next.frame = lock.frame;
    next.position = lock.position;
    next.rate = lock.rate;
    if (lock.epoch != epoch_ || !voice_.reference || voice_.reference != next.reference) {
        if (running_ && voice_.reference) { leaving_ = voice_; crossfade_ = kCrossfadeFrames; }
        voice_ = next;
        epoch_ = lock.epoch;
        if (!running_) {
            running_ = true;
            fade_ = 0;
            restart();
            cutoff_ = lock.cutoff;
            primeModel(toDevice_.identity() ? frame_ : toDevice_.inputFrame(frame_) - toDevice_.half() + 1);
        }
    } else {
        const double difference = next.reference->distance(next.at(generated_), voice_.at(generated_));
        if (std::abs(difference) > kSameVoice) { leaving_ = voice_; crossfade_ = kCrossfadeFrames; }
        voice_ = next;
    }
    locked_ = true;
    fadeStep_ = lock.continued ? 1 / (kContinuedFadeSeconds * rate_) : fadeInStep_;
    levelTarget_ = lock.gain * lock.presence;
    if (lock.cutoff != cutoff_) {
        // Coefficients move over one block; a change that arrives earlier continues from where they are.
        cutoff_ = lock.cutoff;
        lowTarget_ = WindLowPass::design(cutoff_);
        lowRamp_ = kBlock;
    }
}

void WindCanceller::gap(std::int64_t frames) noexcept {
    if (frames > 0) {
        frame_ += frames;
        if (running_) primeModel(toDevice_.identity() ? frame_ : toDevice_.inputFrame(frame_) - toDevice_.half() + 1);
        return;
    }
    ++breaks_;
    locked_ = false;
    fadeStep_ = -1 / (kSwitchSeconds * rate_);
}

// Restarts the 48 kHz model at `first`, letting the low-pass settle on what comes before it.
void WindCanceller::primeModel(std::int64_t first) noexcept {
    crossfade_ = 0;
    low_ = lowTarget_ = WindLowPass::design(cutoff_);
    lowRamp_ = 0;
    lowInput_ = {};
    lowOutput_ = {};
    generated_ = first - kWarmFrames;
    float left = 0, right = 0;
    while (generated_ < first) generate(left, right);
}

// One filtered 48 kHz reference frame into the model ring.
void WindCanceller::generate(float& left, float& right) noexcept {
    float values[2] = {};
    interpolator_->read(*voice_.reference, voice_.at(generated_), values[0], values[1]);
    if (crossfade_ > 0) {
        float old[2] = {};
        interpolator_->read(*leaving_.reference, leaving_.at(generated_), old[0], old[1]);
        const double mix = smooth(static_cast<double>(crossfade_) / static_cast<double>(kCrossfadeFrames));
        for (int channel = 0; channel < 2; ++channel) values[channel] = static_cast<float>(values[channel] + mix * (old[channel] - values[channel]));
        --crossfade_;
    }
    WindLowPass filter = lowTarget_;
    if (lowRamp_ > 0) {
        const double mix = smooth(static_cast<double>(lowRamp_) / static_cast<double>(kBlock));
        filter.b0 += mix * (low_.b0 - filter.b0);
        filter.b1 += mix * (low_.b1 - filter.b1);
        filter.b2 += mix * (low_.b2 - filter.b2);
        filter.a1 += mix * (low_.a1 - filter.a1);
        filter.a2 += mix * (low_.a2 - filter.a2);
        if (--lowRamp_ == 0) low_ = lowTarget_;
    }
    for (std::size_t channel = 0; channel < 2; ++channel) {
        auto& in = lowInput_[channel];
        auto& out = lowOutput_[channel];
        const double x = values[channel];
        const double y = filter.b0 * x + filter.b1 * in[0] + filter.b2 * in[1] - filter.a1 * out[0] - filter.a2 * out[1];
        in[1] = in[0];
        in[0] = x;
        out[1] = out[0];
        out[0] = std::abs(y) < 1e-25 ? 0 : y;
        values[channel] = static_cast<float>(out[0]);
    }
    const auto slot = static_cast<std::size_t>(generated_ & static_cast<std::int64_t>(kModelRing - 1));
    model_[slot * 2] = left = values[0];
    model_[slot * 2 + 1] = right = values[1];
    ++generated_;
}

void WindCanceller::modelAt(std::int64_t frame, float& left, float& right) noexcept {
    if (toDevice_.identity()) {
        generate(left, right);
        return;
    }
    const std::int64_t first = toDevice_.inputFrame(frame) - toDevice_.half() + 1;
    const std::int64_t last = first + static_cast<std::int64_t>(toDevice_.taps());
    while (generated_ < last) { float a = 0, b = 0; generate(a, b); }
    const float* taps = toDevice_.coefficients(frame);
    float sums[2] = {};
    for (std::size_t tap = 0; tap < toDevice_.taps(); ++tap) {
        const auto slot = static_cast<std::size_t>((first + static_cast<std::int64_t>(tap)) & static_cast<std::int64_t>(kModelRing - 1));
        sums[0] += taps[tap] * model_[slot * 2];
        sums[1] += taps[tap] * model_[slot * 2 + 1];
    }
    left = sums[0];
    right = sums[1];
}

void WindCanceller::beginBlock() noexcept {
    levelStep_ = (levelTarget_ - level_) / static_cast<double>(blockFrames_);
    blockInput_ = {};
    blockModel_ = {};
    blockCross_ = {};
}

void WindCanceller::finishBlock() noexcept {
    for (std::size_t band = 0; band < 4; ++band) {
        inputEnergy_[band][energyPosition_] = blockInput_[band];
        modelEnergy_[band][energyPosition_] = blockModel_[band];
        cross_[band][energyPosition_] = blockCross_[band];
    }
    energyPosition_ = (energyPosition_ + 1) % kEnergyBlocks;
    static const double louder = std::pow(10.0, kLouderLimitDb / 10);
    int mismatched = 0;
    for (std::size_t band = 0; band < 4; ++band) {
        double x = 0, e = 0, c = 0;
        for (std::size_t block = 0; block < kEnergyBlocks; ++block) {
            x += inputEnergy_[band][block];
            e += modelEnergy_[band][block];
            c += cross_[band][block];
        }
        bandGain_[band] = bandTarget_[band];
        double gain = std::min(1.0, bandTarget_[band] + kRecoverStep);
        if (e > 0 && x - 2 * gain * c + gain * gain * e > x * louder) gain = std::clamp(2 * c / e, 0.0, 1.0);
        if (e > kMismatchPower && x < kMismatchRatio * e) ++mismatched;
        bandTarget_[band] = gain;
    }
    if (mismatched > kMismatchBands) bandTarget_.fill(0);
}

double WindCanceller::appliedGain() const noexcept {
    if (!running_) return 0;
    double largest = 0;
    for (const double gain : bandTarget_) largest = std::max(largest, gain);
    return largest * smooth(std::clamp(fade_, 0.0, 1.0)) * smooth(activeMix_) * level_;
}

void WindCanceller::process(float* interleaved, std::size_t frames) noexcept {
    for (std::size_t frame = 0; frame < frames; ++frame, ++frame_) {
        activeMix_ = std::clamp(activeMix_ + (active_ ? activeStep_ : -activeStep_), 0.0, 1.0);
        if (!running_) continue;
        fade_ = std::clamp(fade_ + fadeStep_, 0.0, 1.0);
        if (fade_ <= 0 && !locked_) {
            running_ = false;
            voice_ = leaving_ = Voice{};
            crossfade_ = 0;
            continue;
        }
        if (blockPosition_ == 0) beginBlock();
        float model[2] = {};
        modelAt(frame_, model[0], model[1]);
        level_ += levelStep_;
        const double scale = level_ * smooth(fade_) * smooth(activeMix_);
        const double mix = static_cast<double>(blockPosition_) / static_cast<double>(blockFrames_);
        float* out = interleaved + frame * channels_;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double x = std::isfinite(out[channel]) ? out[channel] : 0;
            const double m = model[channel] * scale;
            double inputLow[3], modelLow[3];
            for (std::size_t band = 0; band < 3; ++band) {
                const auto& c = split_[band];
                auto& a = inputSplit_[band][channel];
                inputLow[band] = c.b0 * x + a.z1;
                a.z1 = c.b1 * x - c.a1 * inputLow[band] + a.z2;
                a.z2 = c.b2 * x - c.a2 * inputLow[band];
                auto& b = modelSplit_[band][channel];
                modelLow[band] = c.b0 * m + b.z1;
                b.z1 = c.b1 * m - c.a1 * modelLow[band] + b.z2;
                b.z2 = c.b2 * m - c.a2 * modelLow[band];
            }
            const double input[4] = {inputLow[0], inputLow[1] - inputLow[0], inputLow[2] - inputLow[1], x - inputLow[2]};
            const double bands[4] = {modelLow[0], modelLow[1] - modelLow[0], modelLow[2] - modelLow[1], m - modelLow[2]};
            double removed = 0;
            for (std::size_t band = 0; band < 4; ++band) {
                blockInput_[band] += input[band] * input[band];
                blockModel_[band] += bands[band] * bands[band];
                blockCross_[band] += input[band] * bands[band];
                removed += (bandGain_[band] + mix * (bandTarget_[band] - bandGain_[band])) * bands[band];
            }
            out[channel] = x == 0 ? 0.0f : static_cast<float>(x - removed);
        }
        if (++blockPosition_ == blockFrames_) {
            blockPosition_ = 0;
            finishBlock();
        }
    }
}
} // namespace hikari
