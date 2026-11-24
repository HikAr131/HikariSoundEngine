// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hikari {
// Maximum of the last `length` values. Storage is fixed by prepare(); push() never allocates.
class SlidingMax {
public:
    void prepare(std::size_t length) {
        length_ = length ? length : 1;
        values_.assign(length_ + 1, 0.0f);
        stamps_.assign(length_ + 1, 0);
        reset();
    }
    void reset() noexcept { head_ = tail_ = 0; now_ = 0; }
    float push(float value) noexcept {
        while (tail_ != head_ && values_[previous(tail_)] <= value) tail_ = previous(tail_);
        if (head_ != tail_ && now_ >= length_ && stamps_[head_] <= now_ - length_) head_ = next(head_);
        values_[tail_] = value;
        stamps_[tail_] = now_;
        tail_ = next(tail_);
        ++now_;
        return values_[head_];
    }
private:
    std::size_t next(std::size_t index) const noexcept { return index == length_ ? 0 : index + 1; }
    std::size_t previous(std::size_t index) const noexcept { return index == 0 ? length_ : index - 1; }
    std::size_t length_ = 1, head_ = 0, tail_ = 0;
    std::uint64_t now_ = 0;
    std::vector<float> values_;
    std::vector<std::uint64_t> stamps_;
};
} // namespace hikari
