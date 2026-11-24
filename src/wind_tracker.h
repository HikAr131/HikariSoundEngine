// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "wind_constants.h"
#include "wind_fingerprint.h"
#include "wind_model.h"
#include "wind_reference.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hikari {
// What the canceller renders. `position` is the reference position that belongs to analysis frame `frame`.
struct WindLock {
    std::uint64_t epoch = 0;
    // Breaks of unknown length up to which the position holds.
    std::uint32_t breaks = 0;
    std::int64_t frame = 0;
    double position = 0, rate = 1;
    double gain = 0, cutoff = wind::kNoCutoffHz, presence = 0;
    int reference = -1;
    bool locked = false;
    // A continued lock comes back quickly and an abrupt end leaves quickly; otherwise both fade slowly.
    bool continued = false, abrupt = false;
};

// Finds which reference plays in the 48 kHz analysis stream, where, and how the game has filtered it.
// Runs on one background thread; nothing here is called from the audio thread.
class WindTracker {
public:
    explicit WindTracker(std::shared_ptr<const WindReferenceSet> references);
    void push(const float* stereo, std::size_t frames);
    // A gap of unknown length, the `breaks`-th one: the position has to be found again nearby.
    void discontinuity(std::uint32_t breaks);
    std::int64_t now() const noexcept { return now_; }
    const WindLock& lock() const noexcept { return lock_; }
    std::uint64_t revision() const noexcept { return revision_; }
    double quality() const noexcept { return lock_.locked ? quality_ : 0; }
    bool fruitless() const noexcept { return !lock_.locked && searched_ >= wind::kFruitlessFrames; }
    std::uint64_t searches() const noexcept { return searches_; }
    std::uint64_t losses() const noexcept { return losses_; }
    double positionAt(std::int64_t frame) const noexcept { return lock_.position + static_cast<double>(frame - lock_.frame) * lock_.rate; }

private:
    static constexpr std::size_t kRing = std::size_t(1) << 18;
    struct Candidate { std::size_t reference = 0; double position = 0, score = 0; };
    enum class Verdict { passed, unclear, contradicted };
    struct Alignment { std::int64_t frame = 0; double position = 0, rate = 1, correlation = 0; };
    struct BandPass {
        std::array<double, 2> slow{}, fast{};
        float process(std::size_t channel, float value) noexcept {
            slow[channel] += wind::kConfirmLow * (value - slow[channel]);
            fast[channel] += wind::kConfirmHigh * ((value - slow[channel]) - fast[channel]);
            return static_cast<float>(fast[channel]);
        }
    };
    struct Slot {
        bool used = false;
        std::size_t reference = 0, filled = 0;
        std::int64_t frame = 0;
        double position = 0, rate = 1;
        int passes = 0, unclear = 0;
        double weakest = 1;
        BandPass inputFilter, referenceFilter;
        std::vector<float> input, aligned;
    };
    // Position and rate errors of the running lock.
    struct Loop {
        double pp = 0, pr = 0, rr = 0;
        void reset() noexcept;
        void predict(double frames) noexcept;
        void correct(double offset, double variance, double age, double& position, double& rate) noexcept;
    };

    void copyInput(std::int64_t first, std::size_t frames, float* stereo) const noexcept;
    // The same with moments far louder than usual turned down, so that a shot cannot outweigh a second of wind.
    void copyCalm(std::int64_t first, std::size_t frames, float* stereo) const noexcept;
    void subFrame();
    void block();
    void search();
    void updateUsualPower();
    bool present() const;
    void scan(std::size_t reference, std::vector<Candidate>& output);
    bool coarse(std::size_t reference, std::int64_t frame, std::size_t points, std::size_t radius, double& position) const;
    double fine(std::size_t reference, std::int64_t frame, double& position) const;
    double correlate(std::size_t reference, std::int64_t frame, std::size_t frames, std::size_t radius, double& position);
    std::optional<Alignment> align(std::size_t reference, std::int64_t frame, double position);
    bool mono(std::int64_t frame);
    void consider(std::size_t reference, const Alignment& alignment);
    void runSlot(Slot& slot);
    Verdict confirm(Slot& slot);
    void readAligned(std::size_t reference, double position, double rate, std::size_t frames, float* stereo) const noexcept;
    void beginLock(std::size_t reference, std::int64_t frame, double position, double rate, bool continued);
    void backfill();
    bool beginModel();
    void loseLock(bool abrupt);
    void trackBlock();
    void trackPosition();
    void realign();
    void relocate();
    void move(double offset);
    void publish();

    std::shared_ptr<const WindReferenceSet> references_;
    WindFingerprint fingerprint_;
    WindModel model_;
    std::vector<float> ring_, scratch_, referenceScratch_, inputBlock_, alignedBlock_;
    std::vector<double> scores_;
    std::array<std::array<float, wind::kSubFrames>, wind::kBands> energies_{}, window_{};
    static constexpr std::size_t kPowers = 128;
    std::array<double, kPowers> powers_{};
    double usualPower_ = 0, meanPower_ = 0, kept_ = 0;
    std::array<float, wind::kSubFrames> mask_{};
    std::uint64_t subFrames_ = 0;
    std::int64_t now_ = 0, lastSearch_ = -(std::int64_t(1) << 40);
    std::array<Slot, wind::kMaxSlots> slots_;

    WindLock lock_;
    Loop loop_;
    struct { bool valid = false; std::size_t reference = 0; std::int64_t frame = 0; double position = 0, rate = 1; } last_;
    std::uint64_t revision_ = 0, searches_ = 0, losses_ = 0, blocks_ = 0;
    double quality_ = 0;
    // Where a plain correlation finds the muffled reference, behind the position that is rendered.
    double apparent_ = 0;
    std::int64_t lockedAt_ = 0, lastRealign_ = 0, lastDelay_ = 0, relocateFrom_ = -1;
    std::uint32_t breaks_ = 0;
    std::int64_t searched_ = 0;
    int dropCount_ = 0;
    bool strictReturn_ = false;
    std::array<unsigned char, wind::kQualityBlocks> bad_{};
    std::size_t badPosition_ = 0, badCount_ = 0;
};
} // namespace hikari
