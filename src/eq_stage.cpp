// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "eq_stage.h"
#include "graphic_eq.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::size_t kWarmBlockFrames = 16384;

unsigned checkedChannels(unsigned channels) {
    if (channels < 2 || channels > 8) throw std::invalid_argument("Unsupported history channel count");
    return channels;
}
}

InputHistory::InputHistory(unsigned channels)
    : channels_(checkedChannels(channels)), samples_(kCapacityFrames * channels_) {}

void InputHistory::write(const float* interleaved, std::size_t frames) noexcept {
    const std::uint64_t start = written_.load(std::memory_order_relaxed);
    writeEnd_.store(start + frames, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    auto slot = static_cast<std::size_t>(start % kCapacityFrames);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t run = std::min(frames - done, kCapacityFrames - slot);
        std::memcpy(samples_.data() + slot * channels_, interleaved + done * channels_, run * channels_ * sizeof(float));
        done += run;
        slot = 0;
    }
    written_.store(start + frames, std::memory_order_release);
}

const float* InputHistory::frame(std::uint64_t index) const noexcept {
    return samples_.data() + static_cast<std::size_t>(index % kCapacityFrames) * channels_;
}

bool InputHistory::copy(std::uint64_t first, std::uint64_t last, std::vector<float>& output) const {
    const std::uint64_t available = written();
    if (first > last || last > available || available - first > kCapacityFrames) return false;
    const auto frames = static_cast<std::size_t>(last - first);
    output.resize(frames * channels_);
    auto slot = static_cast<std::size_t>(first % kCapacityFrames);
    for (std::size_t done = 0; done < frames;) {
        const std::size_t run = std::min(frames - done, kCapacityFrames - slot);
        std::memcpy(output.data() + done * channels_, samples_.data() + slot * channels_, run * channels_ * sizeof(float));
        done += run;
        slot = 0;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return writeEnd_.load(std::memory_order_relaxed) - first <= kCapacityFrames;
}

std::unique_ptr<EqChain> EqStage::build(const Parameters& parameters, unsigned sampleRate, unsigned channels) {
    auto chain = std::make_unique<EqChain>();
    chain->config = parameters;
    chain->config.bypass = false;
    chain->filter.configure(chain->config, sampleRate, channels);
    return chain;
}

EqStage::Resources EqStage::allocate(unsigned sampleRate, unsigned channels) {
    Resources resources;
    resources.history = std::make_unique<InputHistory>(channels);
    const auto frames = static_cast<std::size_t>(std::lround(kFadeSeconds * sampleRate));
    if (frames < 2) throw std::invalid_argument("Unsupported fade sample rate");
    resources.fade.resize(frames + 1);
    for (std::size_t position = 0; position <= frames; ++position) {
        const double value = std::sin(kPi * static_cast<double>(position) / (2.0 * static_cast<double>(frames)));
        resources.fade[position] = value * value;
    }
    resources.fade.front() = 0;
    resources.fade.back() = 1;
    resources.scratch.resize(3 * kChunkFrames * channels);
    resources.sampleRate = sampleRate;
    resources.channels = channels;
    return resources;
}

bool EqStage::warm(EqChain& chain, std::vector<float>& scratch) const {
    const std::uint64_t written = history_->written();
    const std::uint64_t needed = chain.filter.warmupFrames(InputHistory::kReadableFrames);
    std::uint64_t start = written > needed ? written - needed : 0;
    // Partition-aligned starts keep a warmed FIR bit-identical to one that never stopped.
    start -= start % GraphicEq::kPartitionFrames;
    chain.consumed = start;
    return catchUp(chain, scratch);
}

bool EqStage::catchUp(EqChain& chain, std::vector<float>& scratch) const {
    for (;;) {
        const std::uint64_t written = history_->written();
        if (chain.consumed == written) return true;
        if (chain.consumed > written) return false;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(written - chain.consumed, kWarmBlockFrames));
        if (!history_->copy(chain.consumed, chain.consumed + count, scratch)) return false;
        chain.filter.process(scratch.data(), count);
        chain.consumed += count;
    }
}

void EqStage::reset(Resources& resources, std::unique_ptr<EqChain> active,
                    std::vector<std::unique_ptr<EqChain>>& released) {
    for (auto* slot : {&active_, &incoming_, &pending_}) if (*slot) released.push_back(std::move(*slot));
    for (auto& slot : retired_) if (slot) released.push_back(std::move(slot));
    std::swap(history_, resources.history);
    std::swap(fade_, resources.fade);
    std::swap(scratch_, resources.scratch);
    channels_ = resources.channels;
    active->consumed = 0;
    active_ = std::move(active);
    fadePosition_ = 0;
}

bool EqStage::retarget(const Parameters& next, std::unique_ptr<EqChain>& released) noexcept {
    if (!active_) return false;
    if (!incoming_) return sameEqualizer(next, active_->config);
    if (sameEqualizer(next, incoming_->config)) {
        released = std::move(pending_);
        return true;
    }
    if (pending_ && sameEqualizer(next, pending_->config)) return true;
    if (!sameEqualizer(next, active_->config)) return false;
    // Turning back mid-fade: the mirrored position keeps the mix continuous.
    released = std::move(pending_);
    std::swap(active_, incoming_);
    fadePosition_ = fadeFrames() - std::min(fadePosition_, fadeFrames());
    record(history_->written(), TransitionRecord::reversed);
    return true;
}

void EqStage::install(std::unique_ptr<EqChain> chain, std::unique_ptr<EqChain>& released) noexcept {
    if (incoming_) {
        released = std::move(pending_);
        pending_ = std::move(chain);
        return;
    }
    incoming_ = std::move(chain);
    fadePosition_ = 0;
    record(history_->written(), TransitionRecord::started);
}

void EqStage::takeRetired(std::vector<std::unique_ptr<EqChain>>& output) {
    for (auto& slot : retired_) if (slot) output.push_back(std::move(slot));
}

void EqStage::record(std::uint64_t frame, unsigned kind) noexcept {
    transitions_[transitionCount_ % kTransitionRecords] = {frame, kind, fadePosition_};
    ++transitionCount_;
}

void EqStage::completeFade(std::uint64_t nextFrame) noexcept {
    const auto free = std::find_if(retired_.begin(), retired_.end(), [](const auto& slot) { return !slot; });
    // With every slot waiting for collection the finished fade keeps rendering the new chain only.
    if (free == retired_.end()) return;
    *free = std::move(active_);
    active_ = std::move(incoming_);
    ++completed_;
    if (!pending_) return;
    incoming_ = std::move(pending_);
    fadePosition_ = 0;
    record(nextFrame, TransitionRecord::started);
}

void EqStage::process(float* samples, std::size_t frames) noexcept {
    if (!active_ || !history_ || frames == 0) return;
    const std::uint64_t base = history_->written();
    history_->write(samples, frames);
    if (!incoming_ && !pending_) {
        active_->filter.process(samples, frames);
        active_->consumed += frames;
        return;
    }
    const std::size_t chunk = kChunkFrames * channels_;
    float* input = scratch_.data();
    float* entering = input + chunk;
    float* queued = entering + chunk;
    const std::size_t total = fadeFrames();
    for (std::size_t done = 0; done < frames;) {
        std::size_t count = std::min(frames - done, kChunkFrames);
        if (incoming_ && fadePosition_ < total) count = std::min(count, total - fadePosition_);
        float* block = samples + done * channels_;
        const std::size_t length = count * channels_;
        std::copy_n(block, length, input);
        active_->filter.process(block, count);
        active_->consumed += count;
        if (pending_) {
            std::copy_n(input, length, queued);
            pending_->filter.process(queued, count);
            pending_->consumed += count;
        }
        if (incoming_) {
            std::copy_n(input, length, entering);
            incoming_->filter.process(entering, count);
            incoming_->consumed += count;
            for (std::size_t frame = 0; frame < count; ++frame) {
                if (fadePosition_ < total) ++fadePosition_;
                float* out = block + frame * channels_;
                const float* next = entering + frame * channels_;
                if (fadePosition_ == total) { std::copy_n(next, channels_, out); continue; }
                const double gain = fade_[fadePosition_];
                for (std::size_t channel = 0; channel < channels_; ++channel)
                    out[channel] = static_cast<float>(out[channel] + gain * (static_cast<double>(next[channel]) - out[channel]));
            }
        }
        done += count;
        if (incoming_ && fadePosition_ == total) completeFade(base + done);
    }
}
} // namespace hikari
