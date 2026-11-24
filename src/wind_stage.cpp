// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "wind_stage.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace hikari {
namespace {
constexpr unsigned kFresh = 4;
constexpr std::size_t kDrainFrames = 1536;
constexpr auto kIdleSleep = std::chrono::milliseconds(8);
constexpr auto kBusySleep = std::chrono::milliseconds(1);
// A file another program holds open for a moment is tried again; one with the wrong content is not.
constexpr int kLoadRetries = 6;
constexpr auto kLoadRetry = std::chrono::milliseconds(500);

// The recordings are large and immutable, so every stage that is alive shares one copy.
std::shared_ptr<const WindReferenceSet> sharedReferences(const std::filesystem::path& path, bool& unreadable, const std::atomic<bool>& stop) {
    static std::mutex mutex;
    static std::weak_ptr<const WindReferenceSet> cache;
    static std::filesystem::path cachedPath;
    std::lock_guard<std::mutex> lock(mutex);
    if (cachedPath == path) if (auto existing = cache.lock()) return existing;
    auto loaded = WindReferenceSet::load(path, WindReferenceSet::publishedDigests(), &unreadable, &stop);
    cache = loaded;
    cachedPath = path;
    return loaded;
}
}

const char* windStateName(WindState state) noexcept {
    switch (state) {
    case WindState::loading: return "loading";
    case WindState::searching: return "searching";
    case WindState::unrecognized: return "unrecognized";
    case WindState::locked: return "locked";
    case WindState::paused: return "paused";
    case WindState::refsInvalid: return "refsInvalid";
    case WindState::off: break;
    }
    return "off";
}

WindStage::WindStage(unsigned rate, unsigned channels, std::filesystem::path references, bool threaded)
    : rate_(rate), channels_(channels), path_(std::move(references)), canceller_(rate, channels),
      toAnalysis_(rate, static_cast<unsigned>(wind::kRate)), ring_(kRingFrames * 2, 0.0f) {
    history_.assign(toAnalysis_.taps() * 2, 0.0f);
    pending_.resize(kDrainFrames * 2);
    if (threaded) thread_ = std::thread([this] { run(); });
}

WindStage::WindStage(unsigned rate, unsigned channels, std::shared_ptr<const WindReferenceSet> references)
    : WindStage(rate, channels, std::filesystem::path(), false) {
    adopt(std::move(references));
}

WindStage::~WindStage() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

void WindStage::adopt(std::shared_ptr<const WindReferenceSet> references) {
    if (!references) {
        show(WindState::refsInvalid);
        return;
    }
    references_ = std::move(references);
    tracker_ = std::make_unique<WindTracker>(references_);
    shared_.store(references_.get(), std::memory_order_release);
    show(WindState::searching);
}

void WindStage::run() {
    try {
        std::shared_ptr<const WindReferenceSet> references;
        auto wait = kLoadRetry;
        for (int attempt = 0; attempt <= kLoadRetries && !references; ++attempt, wait *= 2) {
            bool unreadable = false;
            references = sharedReferences(path_, unreadable, stop_);
            if (references || !unreadable || attempt == kLoadRetries) break;
            for (auto slept = kIdleSleep * 0; slept < wait && !stop_.load(std::memory_order_relaxed); slept += kIdleSleep) std::this_thread::sleep_for(kIdleSleep);
            if (stop_.load(std::memory_order_relaxed)) return;
        }
        adopt(std::move(references));
    } catch (...) { show(WindState::refsInvalid); }
    while (tracker_ && !stop_.load(std::memory_order_relaxed)) {
        const auto waiting = written_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
        try { drain(); } catch (...) {
            // Nobody follows the wind any more, so nothing may be subtracted either.
            publish(WindLock{});
            show(WindState::refsInvalid);
            tracker_.reset();
            return;
        }
        std::this_thread::sleep_for(waiting > kDrainFrames ? kBusySleep : kIdleSleep);
    }
}

void WindStage::pump() {
    while (tracker_ && read_.load(std::memory_order_relaxed) != written_.load(std::memory_order_acquire)) drain();
}

// Audio thread.
void WindStage::gap(std::int64_t frames) noexcept {
    if (frames == 0) return;
    const bool known = frames > 0 && frames <= static_cast<std::int64_t>(rate_);
    canceller_.gap(known ? frames : -1);
    if (known) owed_ += frames;
}

// Audio thread. Both sides count the same frames, so whatever the tracker cannot see is told with its exact length.
bool WindStage::tell(std::uint64_t at) noexcept {
    const auto slot = gapsWritten_.load(std::memory_order_relaxed);
    if (slot - gapsRead_.load(std::memory_order_acquire) >= kGapSlots) return false;
    gaps_[slot % kGapSlots] = {at, owed_, canceller_.breaks()};
    gapsWritten_.store(slot + 1, std::memory_order_release);
    owed_ = 0;
    told_ = canceller_.breaks();
    return true;
}

void WindStage::process(float* interleaved, std::size_t frames) noexcept {
    const auto written = written_.load(std::memory_order_relaxed);
    const auto room = kRingFrames - static_cast<std::size_t>(written - read_.load(std::memory_order_acquire));
    const bool behind = owed_ > 0 || told_ != canceller_.breaks();
    if (room < frames || (behind && !tell(written))) owed_ += static_cast<std::int64_t>(frames);
    else {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto slot = static_cast<std::size_t>(written + frame) & (kRingFrames - 1);
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const float sample = interleaved[frame * channels_ + channel];
                ring_[slot * 2 + channel] = std::isfinite(sample) ? sample : 0.0f;
            }
        }
        written_.store(written + frames, std::memory_order_release);
    }
    if (const auto* references = shared_.load(std::memory_order_acquire)) {
        if (middle_.load(std::memory_order_relaxed) & kFresh) {
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & ~kFresh;
            canceller_.update(references, mailbox_[front_]);
        }
    }
    canceller_.process(interleaved, frames);
}

// Background thread.
void WindStage::drain() {
    auto position = read_.load(std::memory_order_relaxed);
    const auto available = written_.load(std::memory_order_acquire);
    auto nextGap = gapsRead_.load(std::memory_order_relaxed);
    const auto gaps = gapsWritten_.load(std::memory_order_acquire);
    std::uint64_t limit = std::min<std::uint64_t>(available, position + kDrainFrames);
    while (nextGap != gaps && gaps_[nextGap % kGapSlots].at <= position) {
        const auto gap = gaps_[nextGap % kGapSlots];
        std::fill(pending_.begin(), pending_.end(), 0.0f);
        for (std::int64_t left = gap.frames; left > 0 && !stop_.load(std::memory_order_relaxed);) {
            const auto count = static_cast<std::size_t>(std::min<std::int64_t>(left, static_cast<std::int64_t>(kDrainFrames)));
            feed(pending_.data(), count);
            left -= static_cast<std::int64_t>(count);
        }
        if (gap.breaks != seen_) tracker_->discontinuity(seen_ = gap.breaks);
        gapsRead_.store(++nextGap, std::memory_order_release);
    }
    if (nextGap != gaps) limit = std::min(limit, gaps_[nextGap % kGapSlots].at);
    const auto count = static_cast<std::size_t>(limit - position);
    for (std::size_t done = 0; done < count;) {
        const auto slot = static_cast<std::size_t>(position + done) & (kRingFrames - 1);
        const std::size_t run = std::min(count - done, kRingFrames - slot);
        std::copy_n(ring_.data() + slot * 2, run * 2, pending_.data() + done * 2);
        done += run;
    }
    read_.store(limit, std::memory_order_release);
    if (count) feed(pending_.data(), count);
    publish();
}

void WindStage::feed(const float* stereo, std::size_t frames) {
    if (toAnalysis_.identity()) {
        tracker_->push(stereo, frames);
        return;
    }
    const auto span = static_cast<std::int64_t>(toAnalysis_.taps());
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto slot = static_cast<std::size_t>(inputFrames_ % span);
        history_[slot * 2] = stereo[frame * 2];
        history_[slot * 2 + 1] = stereo[frame * 2 + 1];
        ++inputFrames_;
        block_.clear();
        for (;;) {
            const std::int64_t first = toAnalysis_.inputFrame(outputFrames_) - toAnalysis_.half() + 1;
            if (first + span > inputFrames_) break;
            const float* taps = toAnalysis_.coefficients(outputFrames_);
            float sums[2] = {};
            for (std::int64_t tap = 0; tap < span; ++tap) {
                const std::int64_t index = first + tap;
                if (index < 0) continue;
                const auto at = static_cast<std::size_t>(index % span);
                sums[0] += taps[tap] * history_[at * 2];
                sums[1] += taps[tap] * history_[at * 2 + 1];
            }
            block_.push_back(sums[0]);
            block_.push_back(sums[1]);
            ++outputFrames_;
        }
        if (!block_.empty()) tracker_->push(block_.data(), block_.size() / 2);
    }
}

void WindStage::publish(const WindLock& lock) noexcept {
    mailbox_[back_] = lock;
    back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & ~kFresh;
}

void WindStage::publish() noexcept {
    const auto& lock = tracker_->lock();
    if (tracker_->revision() != published_) {
        published_ = tracker_->revision();
        publish(lock);
    }
    show(lock.locked ? WindState::locked : tracker_->fruitless() ? WindState::unrecognized : WindState::searching, lock.locked ? lock.reference : -1);
    statusQuality_.store(std::clamp(tracker_->quality(), 0.0, 1.0), std::memory_order_relaxed);
    statusCutoff_.store(lock.locked ? lock.cutoff : 0, std::memory_order_relaxed);
    statusGain_.store(lock.locked && lock.gain > 1e-5 ? std::clamp(20 * std::log10(lock.gain), -100.0, 20.0) : lock.locked ? -100 : 0, std::memory_order_relaxed);
}

WindStatus WindStage::status() const noexcept {
    WindStatus status;
    const int state = state_.load(std::memory_order_relaxed);
    status.state = static_cast<WindState>(state & 0xFF);
    status.reference = (state >> 8) - 1;
    status.quality = statusQuality_.load(std::memory_order_relaxed);
    status.cutoff = statusCutoff_.load(std::memory_order_relaxed);
    status.gainDb = statusGain_.load(std::memory_order_relaxed);
    return status;
}
} // namespace hikari
