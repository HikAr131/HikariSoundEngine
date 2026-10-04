// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "json.h"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hikari {
struct PlaybackInitializeClass {
    bool failed = false;
    bool exclusive = false;
    const char* code = nullptr;
};
PlaybackInitializeClass classifyPlaybackInitialize(long hr) noexcept;
std::string formatHresult(long hr);

struct PlaybackInitializeReport {
    std::uint64_t sequence = 0;
    long hr = 0;
};
// Sequence of the newest report from the upstream hook; record it as a baseline before upstream runs.
std::uint64_t playbackInitializeSequence() noexcept;
bool takePlaybackInitializeReport(std::uint64_t lastSeen, PlaybackInitializeReport& report) noexcept;

class OutputRecovery {
public:
    enum class Action { none, probe, kick };
    static constexpr std::uint64_t probeIntervalMs = 2000;
    static constexpr std::uint64_t backoffMs[] = {2000, 4000, 8000, 16000, 30000};
    void reset() noexcept;
    bool observe(long hr, std::uint64_t nowMs) noexcept;
    Action poll(std::uint64_t nowMs, bool upstreamParked) noexcept;
    Action probeResult(long hr, std::uint64_t nowMs) noexcept;
    bool active() const noexcept { return failure_ != Failure::none; }
    bool exclusive() const noexcept { return failure_ == Failure::exclusive; }
    long hresult() const noexcept { return hresult_; }
    unsigned probes() const noexcept { return probes_; }
    unsigned kicks() const noexcept { return kicks_; }
    Json lastError(const std::string& deviceDisplayName) const;
private:
    enum class Failure { none, exclusive, internal };
    Action kick(std::uint64_t nowMs) noexcept;
    std::uint64_t backoff() const noexcept { return backoffMs[step_]; }
    Failure failure_ = Failure::none;
    long hresult_ = 0;
    bool parked_ = false;
    bool awaitingReport_ = false;
    std::size_t step_ = 0;
    std::uint64_t dueMs_ = 0;
    unsigned probes_ = 0, kicks_ = 0;
};

std::string visibleEngineState(const std::string& baseState, bool audioRunning, bool paused,
    bool outputFailed, bool bypass, bool processing);
void runOutputRecoveryTests();
}

void hikariOnPlaybackInitializeResult(HRESULT hr) noexcept;
