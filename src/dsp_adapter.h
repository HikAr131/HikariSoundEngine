// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "parameters.h"
#include "eq_stage.h"
#include "DfxDsp.h"
#include <array>
#include <atomic>
#include <cstdint>
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
    };
    DspAdapter();
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
    // Frees chains retired by finished crossfades; never called from the audio thread.
    void collect();
    Stats stats() const noexcept;
    std::size_t fadeFrames();
    double fadeGain(std::size_t position);
    std::vector<TransitionRecord> transitions();
    std::uint64_t completedTransitions();
private:
    class TimedLock;
    int processLocked(float* buffer, int frames, int duplicates);
    bool mixBypass(float* buffer, int frames, std::uint64_t first) noexcept;
    void commitLocked(const Parameters& next);
    std::mutex configurationMutex_;
    std::mutex mutex_;
    std::unique_ptr<DfxDsp> dsp_;
    Parameters parameters_;
    EqStage eq_;
    std::array<double, DfxDsp::NumEffects> effects_{};
    bool bypassTarget_ = false;
    std::size_t bypassPosition_ = 0;
    int sampleRate_ = 0;
    int channels_ = 0;
    std::atomic<std::uint64_t> audioFrames_{0};
    std::atomic<std::uint64_t> chainsBuilt_{0}, upstreamSetterCalls_{0}, syncRetries_{0};
    std::atomic<std::uint64_t> applyLockMaxNanoseconds_{0}, applyLockMaxCycles_{0};
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
