// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "parameters.h"
#include "eq_stage.h"
#include "delta_post.h"
#include "wind_stage.h"
#include "DfxDsp.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace hikari {
class DspAdapter {
public:
    struct Stats {
        std::uint64_t chainsBuilt = 0;
        std::uint64_t upstreamSetterCalls = 0;
        std::uint64_t syncRetries = 0;
        std::uint64_t applyLockMaxNanoseconds = 0;
        std::uint64_t applyLockMaxCycles = 0;
        std::uint64_t upstreamBlocks = 0;
    };
    // Frames the profile adds at one sample rate, stage by stage, minus what skipping the upstream chain saves.
    struct DeltaLatency {
        std::size_t wind = 0, footstep = 0, boom = 0, limiter = 0, upstream = 0;
        std::size_t total() const noexcept { return wind + footstep + boom + limiter; }
    };
    struct DeltaStatus {
        WindStatus wind;
        bool footstepActive = false, limiterWarning = false;
    };
    explicit DspAdapter(std::filesystem::path windReferences = {});
    ~DspAdapter();
    DspAdapter(const DspAdapter&) = delete;
    DspAdapter& operator=(const DspAdapter&) = delete;
    DfxDsp* upstream() noexcept { return dsp_.get(); }
    void apply(const Parameters& parameters);
    void prepareFormat(int sampleRate, int channels);
    Parameters applied();
    std::uint64_t audioFrames() const noexcept { return audioFrames_.load(); }
    int process(float* buffer, int frames, int bits, int channels,
        int sampleRate, int validBits, int duplicates);
    // Frees chains retired by finished crossfades and a wind stage that has faded out; never called from the
    // audio thread.
    void collect();
    Stats stats() const noexcept;
    DeltaStatus deltaStatus();
    DeltaLatency deltaLatency();
    std::size_t fadeFrames();
    double fadeGain(std::size_t position);
    std::vector<TransitionRecord> transitions();
    std::uint64_t completedTransitions();
private:
    class TimedLock;
    // After the wind switch goes off the lock is kept this long, so switching back on continues at once.
    static constexpr std::uint64_t kWindKeepMs = wind::kForgetFrames / 48;
    static constexpr double kDeltaFadeSeconds = 0.020;
    // Warm-up of whichever side is about to be faded in.
    static constexpr double kPrimeSeconds = 0.100;
    int processLocked(float* buffer, int frames, int duplicates);
    int upstreamLocked(float* buffer, int frames, int duplicates);
    bool mixBypass(float* buffer, int frames, std::uint64_t first, const float* original) noexcept;
    void commitLocked(const Parameters& next, std::unique_ptr<WindStage>& wind);
    std::unique_ptr<WindStage> windFor(const Parameters& next, int sampleRate, int channels) const;
    std::mutex configurationMutex_;
    std::mutex mutex_;
    std::unique_ptr<DfxDsp> dsp_;
    const std::filesystem::path windReferences_;
    Parameters parameters_;
    EqStage eq_;
    std::array<double, DfxDsp::NumEffects> effects_{};
    bool bypassTarget_ = false;
    std::size_t bypassPosition_ = 0;
    int sampleRate_ = 0;
    int channels_ = 0;
    std::unique_ptr<WindStage> wind_;
    std::unique_ptr<DeltaPost> post_;
    std::vector<float> original_, deltaBuffer_;
    bool deltaTarget_ = false;
    std::size_t deltaPosition_ = 0, deltaFade_ = 1, prime_ = 0, primeFrames_ = 0;
    std::uint64_t windIdleSince_ = 0;
    std::atomic<std::uint64_t> audioFrames_{0};
    std::atomic<std::uint64_t> chainsBuilt_{0}, upstreamSetterCalls_{0}, syncRetries_{0};
    std::atomic<std::uint64_t> applyLockMaxNanoseconds_{0}, applyLockMaxCycles_{0}, upstreamBlocks_{0};
};

class DspTestRegistry {
public:
    DspTestRegistry();
    ~DspTestRegistry();
    DspTestRegistry(const DspTestRegistry&) = delete;
    DspTestRegistry& operator=(const DspTestRegistry&) = delete;
private:
    std::wstring path_;
};
void runDspTests();
void runTransitionTests();
} // namespace hikari
