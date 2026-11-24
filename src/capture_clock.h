// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <array>
#include <cstdint>

namespace hikari {
// Turns what Windows reports about captured packets into the gaps in front of them. Capture packets and the
// processing call that consumes them run on the same upstream thread.
class CaptureClock {
public:
    // `frames` below zero: nobody knows how much is missing.
    struct Gap { int offset = 0; std::int64_t frames = 0; };
    static constexpr int kMaxGaps = 8;
    static constexpr std::int64_t kMaxKnownGap = 192000;
    static constexpr std::uint64_t kIdleMs = 500;
    static constexpr unsigned long kDiscontinuity = 0x1, kTimestampError = 0x4;

    void packet(int offset, int frames, unsigned long flags, std::uint64_t position, std::uint64_t nowMs) noexcept {
        // A device that had nothing to play, or a new worker thread, goes on at a place nobody knows. The position
        // is only trusted to size a loss that Windows itself reports.
        const bool resumed = !known_ || nowMs - last_ > kIdleMs;
        if (resumed || (flags & kDiscontinuity)) {
            const auto missing = static_cast<std::int64_t>(position - next_);
            const bool sized = !resumed && !(flags & kTimestampError) && missing > 0 && missing <= kMaxKnownGap;
            if (count_ < kMaxGaps) gaps_[static_cast<std::size_t>(count_++)] = {offset, sized ? missing : -1};
            else gaps_.back().frames = -1;
        }
        known_ = true;
        last_ = nowMs;
        next_ = position + static_cast<std::uint64_t>(frames > 0 ? frames : 0);
    }
    int count() const noexcept { return count_; }
    const Gap& gap(int index) const noexcept { return gaps_[static_cast<std::size_t>(index)]; }
    void clear() noexcept { count_ = 0; }

private:
    bool known_ = false;
    std::uint64_t next_ = 0, last_ = 0;
    std::array<Gap, kMaxGaps> gaps_{};
    int count_ = 0;
};
} // namespace hikari
