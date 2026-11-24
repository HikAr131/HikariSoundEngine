// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "resampler.h"
#include "wind_canceller.h"
#include "wind_tracker.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace hikari {
enum class WindState { off, loading, searching, unrecognized, locked, paused, refsInvalid };
struct WindStatus {
    WindState state = WindState::off;
    int reference = -1;
    double quality = 0, cutoff = 0, gainDb = 0;
};
const char* windStateName(WindState state) noexcept;

// Wind cancellation as one insert: the audio thread hands its input to a lock-free ring and renders the last
// published lock; one background thread loads the recordings and tracks. Construction and destruction belong
// to the configuration thread, outside the audio lock.
class WindStage {
public:
    WindStage(unsigned rate, unsigned channels, std::filesystem::path references, bool threaded = true);
    WindStage(unsigned rate, unsigned channels, std::shared_ptr<const WindReferenceSet> references);
    ~WindStage();
    WindStage(const WindStage&) = delete;
    WindStage& operator=(const WindStage&) = delete;

    // Audio thread.
    void setActive(bool active) noexcept { active_ = active; canceller_.setActive(active); }
    void gap(std::int64_t frames) noexcept;
    void process(float* interleaved, std::size_t frames) noexcept;
    bool silent() const noexcept { return canceller_.silent(); }

    WindStatus status() const noexcept;
    // Runs the tracker over everything received so far; only for a stage without its own thread.
    void pump();
    const WindTracker* tracker() const noexcept { return tracker_.get(); }
    const WindCanceller& canceller() const noexcept { return canceller_; }

private:
    static constexpr std::size_t kRingFrames = std::size_t(1) << 18;
    static constexpr std::size_t kGapSlots = 64;
    struct Gap { std::uint64_t at = 0; std::int64_t frames = 0; std::uint32_t breaks = 0; };
    void adopt(std::shared_ptr<const WindReferenceSet> references);
    void run();
    void drain();
    void feed(const float* stereo, std::size_t frames);
    bool tell(std::uint64_t at) noexcept;
    void publish() noexcept;
    void publish(const WindLock& lock) noexcept;
    void show(WindState state, int reference = -1) noexcept { state_.store(static_cast<int>(state) | (reference + 1) << 8, std::memory_order_relaxed); }

    const unsigned rate_, channels_;
    const std::filesystem::path path_;
    WindCanceller canceller_;
    RationalResampler toAnalysis_;
    bool active_ = true;

    // Audio thread to background: interleaved stereo plus the gaps between them.
    std::vector<float> ring_;
    std::atomic<std::uint64_t> written_{0}, read_{0};
    std::array<Gap, kGapSlots> gaps_{};
    std::atomic<std::uint32_t> gapsWritten_{0}, gapsRead_{0};
    // Frames of the common timeline and breaks the tracker has not been told about yet.
    std::int64_t owed_ = 0;
    std::uint32_t told_ = 0;
    std::uint32_t seen_ = 0;

    // Background to audio thread: the newest lock only.
    std::array<WindLock, 3> mailbox_{};
    std::atomic<unsigned> middle_{1};
    unsigned back_ = 0, front_ = 2;
    std::atomic<const WindReferenceSet*> shared_{nullptr};

    std::shared_ptr<const WindReferenceSet> references_;
    std::unique_ptr<WindTracker> tracker_;
    std::uint64_t published_ = 0;
    std::vector<float> history_, pending_, block_;
    std::int64_t inputFrames_ = 0, outputFrames_ = 0;

    // Low byte: WindState, above it the reference plus one.
    std::atomic<int> state_{static_cast<int>(WindState::loading)};
    std::atomic<double> statusQuality_{0}, statusCutoff_{0}, statusGain_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
} // namespace hikari
