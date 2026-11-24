// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "resampler.h"
#include "wind_constants.h"
#include "wind_model.h"
#include "wind_reference.h"
#include "wind_tracker.h"
#include <array>
#include <cstdint>
#include <vector>

namespace hikari {
// Renders the locked reference the way the game plays it and subtracts it from the first two channels, split
// into four bands that each guard against making things worse. The model is known ahead of time, so nothing is
// delayed; without a lock the audio is left untouched. Only the constructor allocates.
class WindCanceller {
public:
    WindCanceller(unsigned rate, unsigned channels);
    // Audio thread. `references` stays valid for as long as a lock refers to it.
    void update(const WindReferenceSet* references, const WindLock& lock) noexcept;
    void setActive(bool active) noexcept { active_ = active; }
    // The input continues `frames` later; a negative count means nobody knows.
    void gap(std::int64_t frames) noexcept;
    void process(float* interleaved, std::size_t frames) noexcept;
    bool silent() const noexcept { return fade_ <= 0 || activeMix_ <= 0; }
    std::int64_t frame() const noexcept { return frame_; }
    std::uint32_t breaks() const noexcept { return breaks_; }
    double bandGain(std::size_t band) const noexcept { return bandTarget_[band]; }
    // Largest share of the model still being subtracted.
    double appliedGain() const noexcept;

private:
    static constexpr std::size_t kModelRing = 512;
    struct Voice {
        const WindReference* reference = nullptr;
        std::int64_t frame = 0;
        double position = 0, rate = 1;
        double at(std::int64_t model) const noexcept { return position + static_cast<double>(model - frame) * rate; }
    };
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct Memory { double z1 = 0, z2 = 0; };
    void generate(float& left, float& right) noexcept;
    void primeModel(std::int64_t first) noexcept;
    void modelAt(std::int64_t frame, float& left, float& right) noexcept;
    void finishBlock() noexcept;
    void beginBlock() noexcept;
    void restart() noexcept;

    unsigned rate_, channels_;
    std::size_t blockFrames_, blockPosition_ = 0;
    RationalResampler toDevice_;
    const WindInterpolator* interpolator_ = nullptr;
    std::int64_t frame_ = 0;

    Voice voice_, leaving_;
    std::size_t crossfade_ = 0;
    std::uint64_t epoch_ = 0;
    std::uint32_t breaks_ = 0;
    bool locked_ = false, active_ = true, running_ = false;
    double fade_ = 0, fadeInStep_ = 0, fadeOutStep_ = 0, fadeStep_ = 0, activeMix_ = 1, activeStep_ = 0;
    double level_ = 0, levelTarget_ = 0, levelStep_ = 0;

    WindLowPass low_, lowTarget_;
    double cutoff_ = wind::kNoCutoffHz;
    std::size_t lowRamp_ = 0;
    std::array<std::array<double, 2>, 2> lowInput_{}, lowOutput_{};
    std::vector<float> model_;
    std::int64_t generated_ = 0;

    std::array<Biquad, 3> split_{};
    std::array<std::array<Memory, 2>, 3> inputSplit_{}, modelSplit_{};
    std::array<std::array<double, wind::kEnergyBlocks>, 4> inputEnergy_{}, modelEnergy_{}, cross_{};
    std::size_t energyPosition_ = 0;
    std::array<double, 4> blockInput_{}, blockModel_{}, blockCross_{}, bandGain_{}, bandTarget_{};
};
} // namespace hikari
