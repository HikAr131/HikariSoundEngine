// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hikari {
// Zero-phase windowed-sinc conversion between two integer rates. Output frame k lies exactly at input time
// k * inputRate / outputRate, so both sides share one timeline and no fractional delay appears.
class RationalResampler {
public:
    RationalResampler(unsigned inputRate, unsigned outputRate);
    bool identity() const noexcept { return up_ == 1 && down_ == 1; }
    // Frames needed on each side: taps cover [first, first + taps) with first = inputFrame - half + 1.
    std::int64_t half() const noexcept { return half_; }
    std::size_t taps() const noexcept { return static_cast<std::size_t>(2 * half_); }
    std::int64_t inputFrame(std::int64_t outputFrame) const noexcept { return floorDiv(outputFrame * down_, up_); }
    // First output frame at or after the given input frame.
    std::int64_t outputFrame(std::int64_t inputFrame) const noexcept { return -floorDiv(-inputFrame * up_, down_); }
    const float* coefficients(std::int64_t outputFrame) const noexcept {
        const std::int64_t scaled = outputFrame * down_;
        return table_.data() + static_cast<std::size_t>(scaled - floorDiv(scaled, up_) * up_) * taps();
    }
private:
    static std::int64_t floorDiv(std::int64_t value, std::int64_t divisor) noexcept {
        const std::int64_t quotient = value / divisor;
        return quotient * divisor > value ? quotient - 1 : quotient;
    }
    std::int64_t up_ = 1, down_ = 1, half_ = 1;
    std::vector<float> table_;
};
} // namespace hikari
