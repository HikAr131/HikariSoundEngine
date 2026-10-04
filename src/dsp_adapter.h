// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "parameters.h"
#include "parametric_eq.h"
#include "DfxDsp.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace hikari {
class DspAdapter {
public:
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
private:
    std::mutex configurationMutex_;
    std::mutex mutex_;
    std::unique_ptr<DfxDsp> dsp_;
    Parameters parameters_;
    ParametricEq equalizer_;
    int sampleRate_ = 0;
    int channels_ = 0;
    std::atomic<std::uint64_t> audioFrames_{0};
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
} // namespace hikari
