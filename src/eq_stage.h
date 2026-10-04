// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "parameters.h"
#include "parametric_eq.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace hikari {
// A configured own-segment filter, the parameters it renders and the input frames it consumed.
struct EqChain {
    Parameters config;
    ParametricEq filter;
    std::uint64_t consumed = 0;
};

// Recent pre-EQ input. The audio thread writes it while holding the audio lock; the
// configuration thread copies from it without that lock and rejects any copy that a
// published or in-flight write could have reached.
class InputHistory {
public:
    static constexpr std::size_t kReadableFrames = 65536;
    static constexpr std::size_t kCapacityFrames = kReadableFrames + 32768;
    static constexpr std::size_t kMaxWriteFrames = 16384;
    explicit InputHistory(unsigned channels);
    InputHistory(const InputHistory&) = delete;
    InputHistory& operator=(const InputHistory&) = delete;
    unsigned channels() const noexcept { return channels_; }
    std::uint64_t written() const noexcept { return written_.load(std::memory_order_acquire); }
    void write(const float* interleaved, std::size_t frames) noexcept;
    const float* frame(std::uint64_t index) const noexcept;
    bool copy(std::uint64_t first, std::uint64_t last, std::vector<float>& output) const;
private:
    unsigned channels_;
    std::vector<float> samples_;
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> writeEnd_{0};
};

struct TransitionRecord {
    enum Kind : unsigned { started = 1, reversed = 2 };
    std::uint64_t frame = 0;
    unsigned kind = 0;
    std::size_t position = 0;
};

// Own preamp/EQ/treble segment with click-free parameter changes. A replacement chain is
// built and warmed from the input history without the audio lock, then crossfaded in.
class EqStage {
public:
    static constexpr std::size_t kChunkFrames = 256;
    static constexpr std::size_t kRetiredSlots = 8;
    static constexpr std::size_t kTransitionRecords = 64;
    static constexpr double kFadeSeconds = 0.020;

    struct Resources {
        std::unique_ptr<InputHistory> history;
        std::vector<double> fade;
        std::vector<float> scratch;
        unsigned sampleRate = 0, channels = 0;
    };

    // Configuration thread, without the audio lock.
    static std::unique_ptr<EqChain> build(const Parameters& parameters, unsigned sampleRate, unsigned channels);
    static Resources allocate(unsigned sampleRate, unsigned channels);
    bool warm(EqChain& chain, std::vector<float>& scratch) const;
    bool catchUp(EqChain& chain, std::vector<float>& scratch) const;

    // Audio lock held. Chains and buffers leave through the output arguments so that they
    // are destroyed after the lock is released.
    void reset(Resources& resources, std::unique_ptr<EqChain> active,
               std::vector<std::unique_ptr<EqChain>>& released);
    bool ready() const noexcept { return static_cast<bool>(active_); }
    bool synced(const EqChain& chain) const noexcept { return history_ && chain.consumed == history_->written(); }
    bool retarget(const Parameters& next, std::unique_ptr<EqChain>& released) noexcept;
    void install(std::unique_ptr<EqChain> chain, std::unique_ptr<EqChain>& released) noexcept;
    void takeRetired(std::vector<std::unique_ptr<EqChain>>& output);
    void catchUpLocked(EqChain& chain, std::vector<float>& scratch);
    void process(float* interleaved, std::size_t frames) noexcept;
    std::uint64_t written() const noexcept { return history_ ? history_->written() : 0; }
    const float* dry(std::uint64_t frame) const noexcept { return history_->frame(frame); }
    std::size_t fadeFrames() const noexcept { return fade_.empty() ? 0 : fade_.size() - 1; }
    double fadeGain(std::size_t position) const noexcept { return fade_[position]; }
    std::uint64_t transitionCount() const noexcept { return transitionCount_; }
    TransitionRecord transition(std::uint64_t index) const noexcept { return transitions_[index % kTransitionRecords]; }
    std::uint64_t completedTransitions() const noexcept { return completed_; }

private:
    void completeFade(std::uint64_t nextFrame) noexcept;
    void record(std::uint64_t frame, unsigned kind) noexcept;
    std::unique_ptr<InputHistory> history_;
    std::vector<double> fade_;
    std::vector<float> scratch_;
    unsigned sampleRate_ = 0, channels_ = 0;
    std::unique_ptr<EqChain> active_, incoming_, pending_;
    std::array<std::unique_ptr<EqChain>, kRetiredSlots> retired_{};
    std::array<TransitionRecord, kTransitionRecords> transitions_{};
    std::size_t fadePosition_ = 0;
    std::uint64_t transitionCount_ = 0, completed_ = 0;
};
} // namespace hikari
