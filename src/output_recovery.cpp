// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "output_recovery.h"
#include <audioclient.h>
#include <cstdio>
#include <iterator>
#include <mutex>

namespace {
std::mutex playbackReportMutex;
hikari::PlaybackInitializeReport playbackReport;
}

void hikariOnPlaybackInitializeResult(HRESULT hr) noexcept {
    try {
        std::lock_guard<std::mutex> lock(playbackReportMutex);
        playbackReport = {playbackReport.sequence + 1, hr};
    } catch (...) {}
}

namespace hikari {
PlaybackInitializeClass classifyPlaybackInitialize(long hr) noexcept {
    if (SUCCEEDED(hr)) return {};
    if (hr == AUDCLNT_E_DEVICE_IN_USE) return {true, true, "OUTPUT_EXCLUSIVE_LOCKED"};
    return {true, false, "INTERNAL"};
}
std::string formatHresult(long hr) {
    char text[11] = {};
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}
std::uint64_t playbackInitializeSequence() noexcept {
    std::lock_guard<std::mutex> lock(playbackReportMutex);
    return playbackReport.sequence;
}
bool takePlaybackInitializeReport(std::uint64_t lastSeen, PlaybackInitializeReport& report) noexcept {
    std::lock_guard<std::mutex> lock(playbackReportMutex);
    if (playbackReport.sequence <= lastSeen) return false;
    report = playbackReport;
    return true;
}

void OutputRecovery::reset() noexcept { *this = OutputRecovery(); }
bool OutputRecovery::observe(long hr, std::uint64_t nowMs) noexcept {
    const auto kind = classifyPlaybackInitialize(hr);
    const auto next = !kind.failed ? Failure::none : (kind.exclusive ? Failure::exclusive : Failure::internal);
    const bool changed = next != failure_ || (next != Failure::none && hr != hresult_);
    if (next != Failure::none && failure_ != Failure::none && next != failure_) step_ = 0;
    failure_ = next;
    hresult_ = next == Failure::none ? 0 : hr;
    awaitingReport_ = false;
    // A success while upstream stays parked falls back to the unreported-park schedule; the step only
    // resets once upstream runs, so a failure after the playback Initialize cannot retry every 2 s.
    dueMs_ = nowMs + (next == Failure::exclusive ? probeIntervalMs : backoff());
    return changed;
}
OutputRecovery::Action OutputRecovery::poll(std::uint64_t nowMs, bool upstreamParked) noexcept {
    if (!upstreamParked) { parked_ = false; step_ = 0; return Action::none; }
    const bool probing = failure_ == Failure::exclusive && !awaitingReport_;
    if (!parked_) { parked_ = true; dueMs_ = nowMs + (probing ? probeIntervalMs : backoff()); }
    if (nowMs < dueMs_) return Action::none;
    if (!probing) return kick(nowMs);
    ++probes_;
    dueMs_ = nowMs + probeIntervalMs;
    return Action::probe;
}
OutputRecovery::Action OutputRecovery::probeResult(long hr, std::uint64_t nowMs) noexcept {
    // The next probe was scheduled when this one was issued.
    if (failure_ != Failure::exclusive || awaitingReport_ || classifyPlaybackInitialize(hr).exclusive) return Action::none;
    return kick(nowMs);
}
OutputRecovery::Action OutputRecovery::kick(std::uint64_t nowMs) noexcept {
    ++kicks_;
    if (step_ + 1 < std::size(backoffMs)) ++step_;
    // Probing pauses until the retried upstream reinit reports again.
    awaitingReport_ = true;
    dueMs_ = nowMs + backoff();
    return Action::kick;
}
Json OutputRecovery::lastError(const std::string& deviceDisplayName) const {
    if (!active()) return Json();
    return Json::object({{"ok", false}, {"code", classifyPlaybackInitialize(hresult_).code},
        {"message", exclusive() ? "Output endpoint is in exclusive use by another application" : "Audio output initialization failed"},
        {"hresult", formatHresult(hresult_)}, {"device", deviceDisplayName}});
}

bool outputReady(bool outputInitialized, bool upstreamParked, bool virtualIsDefault, bool noDefaultSwitch) noexcept {
    return outputInitialized && !upstreamParked && (virtualIsDefault || noDefaultSwitch);
}
std::string visibleEngineState(const std::string& baseState, bool audioRunning, bool paused,
    bool outputFailed, bool bypass, bool processing, bool ready) {
    if (!audioRunning || baseState == "yielded" || paused) return baseState;
    if (outputFailed) return "idle-no-device";
    if (bypass) return "bypassed";
    if (processing) return "processing";
    return ready ? "ready" : "starting";
}
}
