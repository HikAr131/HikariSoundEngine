// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "output_recovery.h"
#include <audioclient.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hikari {
namespace {
using Action = OutputRecovery::Action;
using Times = std::vector<std::uint64_t>;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
Times intervals(const Times& times, std::uint64_t origin) {
    Times result;
    for (auto time : times) { result.push_back(time - origin); origin = time; }
    return result;
}
// Drives parked 100 ms ticks; an INTERNAL report follows every kick on the next tick, as upstream reinit does.
Times internalKickDelays(OutputRecovery& recovery, long hr, std::uint64_t& now, std::size_t count) {
    Times delays;
    std::uint64_t reportedAt = now;
    bool reportDue = false;
    for (const auto limit = now + 300000; delays.size() < count && now < limit;) {
        now += 100;
        if (reportDue) {
            require(!recovery.observe(hr, now), "Repeated INTERNAL report changed the visible state");
            reportedAt = now; reportDue = false;
        }
        const auto action = recovery.poll(now, true);
        require(action != Action::probe, "INTERNAL failure requested a probe");
        if (action == Action::kick) { delays.push_back(now - reportedAt); reportDue = true; }
    }
    return delays;
}
std::uint64_t firstAction(OutputRecovery& recovery, std::uint64_t& now, std::uint64_t window, Action wanted) {
    for (const auto limit = now + window; now <= limit; now += 100) {
        const auto action = recovery.poll(now, true);
        if (action == wanted) return now;
        require(action == Action::none, "Unexpected recovery action");
    }
    return 0;
}
}

void runOutputRecoveryTests() {
    static_assert(AUDCLNT_E_DEVICE_IN_USE == static_cast<HRESULT>(0x8889000AL), "Unexpected AUDCLNT_E_DEVICE_IN_USE");
    struct Row { long hr; const char* code; const char* hex; };
    const Row table[] = {
        {S_OK, nullptr, "0x00000000"}, {S_FALSE, nullptr, "0x00000001"},
        {AUDCLNT_E_DEVICE_IN_USE, "OUTPUT_EXCLUSIVE_LOCKED", "0x8889000A"},
        {AUDCLNT_E_UNSUPPORTED_FORMAT, "INTERNAL", "0x88890008"},
        {AUDCLNT_E_DEVICE_INVALIDATED, "INTERNAL", "0x88890004"},
        {AUDCLNT_E_SERVICE_NOT_RUNNING, "INTERNAL", "0x88890010"},
        {E_FAIL, "INTERNAL", "0x80004005"}, {E_OUTOFMEMORY, "INTERNAL", "0x8007000E"}};
    for (const auto& row : table) {
        const auto kind = classifyPlaybackInitialize(row.hr);
        require(kind.failed == (row.code != nullptr) && kind.exclusive == (row.hr == AUDCLNT_E_DEVICE_IN_USE),
            "Playback Initialize classification failed");
        require(row.code ? kind.code && std::string(kind.code) == row.code : kind.code == nullptr,
            "Playback Initialize error code mismatch");
        require(formatHresult(row.hr) == row.hex, "HRESULT formatting failed");
    }
    require(formatHresult(0x7fffffffL) == "0x7FFFFFFF" && formatHresult(-1L) == "0xFFFFFFFF", "HRESULT formatting range failed");

    PlaybackInitializeReport report;
    hikariOnPlaybackInitializeResult(E_FAIL);
    const auto baseline = playbackInitializeSequence();
    require(!takePlaybackInitializeReport(baseline, report), "Report older than the baseline was taken");
    hikariOnPlaybackInitializeResult(AUDCLNT_E_DEVICE_IN_USE);
    require(takePlaybackInitializeReport(baseline, report) && report.sequence == baseline + 1 &&
        report.hr == AUDCLNT_E_DEVICE_IN_USE, "Hook report was not delivered");
    require(!takePlaybackInitializeReport(report.sequence, report), "Consumed report was delivered again");
    require(takePlaybackInitializeReport(baseline - 1, report) && report.sequence == baseline + 1,
        "Mailbox did not keep the newest report");

    OutputRecovery recovery;
    std::uint64_t now = 10000;
    require(recovery.poll(now, true) == Action::none, "Fresh recovery acted immediately");
    require(recovery.observe(report.hr, now), "Exclusive failure was not visible");
    require(!recovery.observe(report.hr, now), "Repeated exclusive report changed the visible state");
    require(recovery.active() && recovery.exclusive() && recovery.hresult() == AUDCLNT_E_DEVICE_IN_USE, "Exclusive state mismatch");
    const std::string locked = R"j({"code":"OUTPUT_EXCLUSIVE_LOCKED","device":"Speakers (Test)","hresult":"0x8889000A",)j"
        R"j("message":"Output endpoint is in exclusive use by another application","ok":false})j";
    require(recovery.lastError("Speakers (Test)").stringify() == locked, "Exclusive lastError shape mismatch");
    for (bool bypass : {false, true}) for (bool processing : {false, true}) for (bool ready : {false, true})
        require(visibleEngineState("starting", true, false, recovery.active(), bypass, processing, ready) == "idle-no-device",
            "Output failure did not report idle-no-device");
    require(visibleEngineState("starting", true, false, false, false, false, false) == "starting" &&
        visibleEngineState("starting", true, false, false, false, false, true) == "ready" &&
        visibleEngineState("starting", true, false, false, false, true, false) == "processing" &&
        visibleEngineState("starting", true, false, false, false, true, true) == "processing" &&
        visibleEngineState("starting", true, false, false, true, true, true) == "bypassed" &&
        visibleEngineState("starting", true, false, false, true, false, true) == "bypassed" &&
        visibleEngineState("yielded", true, false, true, false, true, true) == "yielded" &&
        visibleEngineState("starting", true, true, true, false, true, true) == "starting" &&
        visibleEngineState("idle-no-device", false, false, true, false, false, true) == "idle-no-device" &&
        visibleEngineState("conflict-official-fxsound", false, false, false, true, false, true) == "conflict-official-fxsound",
        "Visible engine state mapping changed");
    // Ready needs an opened output, a running upstream and the virtual default unless the default is left alone.
    unsigned readyCombinations = 0;
    for (bool initialized : {false, true}) for (bool parked : {false, true})
        for (bool virtualDefault : {false, true}) for (bool noSwitch : {false, true}) {
            const bool expected = initialized && !parked && (virtualDefault || noSwitch);
            require(outputReady(initialized, parked, virtualDefault, noSwitch) == expected, "Ready condition mismatch");
            readyCombinations += expected ? 1 : 0;
        }
    require(readyCombinations == 3, "Ready condition truth table changed");

    // 60 simulated seconds of an exclusive lock: probes every 2000 ms, never a reinit.
    Times probes;
    unsigned kicks = 0;
    const auto lockedAt = now;
    for (now = lockedAt + 100; now <= lockedAt + 60000; now += 100) {
        const auto action = recovery.poll(now, true);
        if (action == Action::kick) ++kicks;
        if (action != Action::probe) continue;
        probes.push_back(now);
        require(recovery.probeResult(AUDCLNT_E_DEVICE_IN_USE, now) == Action::none, "Locked probe requested a reinit");
    }
    require(probes.size() == 30 && kicks == 0 && recovery.probes() == 30 && recovery.kicks() == 0, "Exclusive probe count mismatch");
    require(intervals(probes, lockedAt) == Times(30, 2000), "Exclusive probe cadence mismatch");
    require(recovery.active() && recovery.exclusive(), "Exclusive lock lost while probes failed");
    for (const auto until = now + 5000; now < until; now += 100)
        require(recovery.poll(now, false) == Action::none, "Recovery acted while upstream was not parked");
    const auto reparked = now;
    require(firstAction(recovery, now, 2000, Action::probe) == reparked + 2000, "Probing did not resume 2000 ms after parking");

    // Release: one kick, then the retried reinit reports S_OK through the real hook.
    require(recovery.probeResult(S_OK, now) == Action::kick && recovery.kicks() == 1, "Released lock did not request one reinit");
    const auto kickedAt = now;
    for (now += 100; now < kickedAt + 1000; now += 100)
        require(recovery.poll(now, true) == Action::none, "Released lock requested more than one reinit");
    const auto beforeSuccess = playbackInitializeSequence();
    hikariOnPlaybackInitializeResult(S_OK);
    require(takePlaybackInitializeReport(beforeSuccess, report) && report.hr == S_OK, "Success report was not delivered");
    require(recovery.observe(report.hr, now), "Success did not clear the output failure");
    require(!recovery.active() && !recovery.exclusive() && recovery.hresult() == S_OK &&
        recovery.lastError("Speakers (Test)").isNull(), "Success left output failure state");
    require(visibleEngineState("starting", true, false, recovery.active(), false, false, false) == "starting" &&
        visibleEngineState("starting", true, false, recovery.active(), false, false, true) == "ready" &&
        visibleEngineState("starting", true, false, recovery.active(), false, true, true) == "processing", "Success did not restore engine state");
    for (const auto until = now + 60000; now < until; now += 100)
        require(recovery.poll(now, false) == Action::none, "Running upstream was retried");
    require(recovery.kicks() == 1 && recovery.probes() == 31, "Recovery counters mismatch");

    // INTERNAL failures back off 2/4/8/16/30/30 s between the failure report and the reinit.
    recovery.reset();
    require(!recovery.active() && recovery.hresult() == S_OK && recovery.probes() == 0 && recovery.kicks() == 0 &&
        recovery.lastError("Speakers (Test)").isNull(), "Reset kept recovery state");
    now = 200000;
    require(recovery.observe(E_FAIL, now) && recovery.active() && !recovery.exclusive() && recovery.hresult() == E_FAIL,
        "INTERNAL failure was not visible");
    require(recovery.poll(now, true) == Action::none, "INTERNAL failure retried immediately");
    const std::string internal = R"j({"code":"INTERNAL","device":"Speakers (Test)","hresult":"0x80004005",)j"
        R"j("message":"Audio output initialization failed","ok":false})j";
    require(recovery.lastError("Speakers (Test)").stringify() == internal, "INTERNAL lastError shape mismatch");
    require(internalKickDelays(recovery, E_FAIL, now, 6) == Times{2000, 4000, 8000, 16000, 30000, 30000},
        "INTERNAL backoff sequence mismatch");
    require(recovery.kicks() == 6 && recovery.probes() == 0, "INTERNAL counters mismatch");
    for (const auto until = now + 60000; now < until; now += 100)
        require(recovery.poll(now, false) == Action::none, "INTERNAL retry ran while upstream was not parked");
    require(recovery.observe(E_OUTOFMEMORY, now) && !recovery.exclusive() &&
        recovery.lastError("Speakers (Test)").at("hresult").asString() == "0x8007000E", "INTERNAL HRESULT change was not visible");
    const auto restartedAt = now;
    require(firstAction(recovery, now, 2000, Action::kick) == restartedAt + 2000, "INTERNAL backoff did not restart after upstream ran");

    // Reclassification resets the backoff; an INTERNAL HRESULT change within the same class does not.
    recovery.reset();
    now = 400000;
    recovery.observe(AUDCLNT_E_DEVICE_IN_USE, now);
    require(firstAction(recovery, now, 2000, Action::probe) == 402000, "Exclusive probe was not scheduled");
    require(recovery.probeResult(AUDCLNT_E_DEVICE_INVALIDATED, now) == Action::kick, "Non-lock probe failure did not request a reinit");
    now += 100;
    require(recovery.observe(AUDCLNT_E_DEVICE_INVALIDATED, now) && recovery.active() && !recovery.exclusive() &&
        recovery.lastError("Speakers (Test)").at("code").asString() == "INTERNAL" &&
        recovery.lastError("Speakers (Test)").at("hresult").asString() == "0x88890004", "Exclusive to INTERNAL reclassification failed");
    auto reclassifiedAt = now;
    require(firstAction(recovery, now, 2000, Action::kick) == reclassifiedAt + 2000, "Reclassification did not reset the backoff");
    now += 100;
    require(recovery.observe(E_FAIL, now), "INTERNAL HRESULT change was not visible");
    reclassifiedAt = now;
    require(firstAction(recovery, now, 4000, Action::kick) == reclassifiedAt + 4000, "Same-class HRESULT change reset the backoff");
    now += 100;
    require(recovery.observe(AUDCLNT_E_DEVICE_IN_USE, now) && recovery.exclusive(), "INTERNAL to exclusive reclassification failed");
    reclassifiedAt = now;
    require(firstAction(recovery, now, 2000, Action::probe) == reclassifiedAt + 2000, "Reclassified lock did not probe");

    // A probe-requested reinit that does not report again falls back to backoff kicks, not probes.
    require(recovery.probeResult(S_OK, now) == Action::kick, "Released lock did not request a reinit");
    const auto silentAt = now;
    now += 100;
    require(firstAction(recovery, now, 4000, Action::kick) == silentAt + 4000, "Unreported reinit was not backed off");
    require(recovery.exclusive(), "Unreported reinit changed the visible failure");

    // A lock taken again right after a release keeps the 2000 ms probe cadence despite the kick's backoff step.
    recovery.reset();
    now = 500000;
    recovery.observe(AUDCLNT_E_DEVICE_IN_USE, now);
    require(firstAction(recovery, now, 2000, Action::probe) == 502000, "Exclusive probe was not scheduled");
    require(recovery.probeResult(S_OK, now) == Action::kick, "Released lock did not request a reinit");
    now += 100;
    require(!recovery.observe(AUDCLNT_E_DEVICE_IN_USE, now) && recovery.exclusive(), "Lock taken again changed the visible state");
    const auto relockedAt = now;
    Times relockProbes;
    for (const auto until = now + 10000; now <= until; now += 100) {
        const auto action = recovery.poll(now, true);
        require(action != Action::kick, "Lock taken again requested a reinit");
        if (action != Action::probe) continue;
        relockProbes.push_back(now);
        require(recovery.probeResult(AUDCLNT_E_DEVICE_IN_USE, now) == Action::none, "Locked probe requested a reinit");
    }
    require(intervals(relockProbes, relockedAt) == Times(5, 2000), "Probing slowed after the lock was taken again");

    // Parked without any failure report: kicks back off from the start of continuous parking, no lastError.
    recovery.reset();
    now = 600000;
    for (const auto until = now + 1500; now < until; now += 100)
        require(recovery.poll(now, true) == Action::none, "Short park requested a reinit");
    require(recovery.poll(now, false) == Action::none, "Running upstream was retried");
    now += 100;
    const auto parkedAt = now;
    Times generic;
    for (const auto limit = now + 200000; generic.size() < 6 && now < limit; now += 100) {
        const auto action = recovery.poll(now, true);
        require(action != Action::probe, "Unreported park requested a probe");
        if (action == Action::kick) generic.push_back(now);
        require(!recovery.active() && recovery.lastError("Speakers (Test)").isNull(), "Unreported park created a lastError");
    }
    require(intervals(generic, parkedAt) == Times{2000, 4000, 8000, 16000, 30000, 30000}, "Unreported park backoff mismatch");
    require(visibleEngineState("starting", true, false, recovery.active(), false, false, false) == "starting", "Unreported park changed engine state");

    // A success while upstream stays parked keeps the backoff until upstream runs.
    recovery.reset();
    now = 900000;
    recovery.observe(E_FAIL, now);
    require(firstAction(recovery, now, 2000, Action::kick) == 902000, "INTERNAL kick was not scheduled");
    now += 100;
    require(recovery.observe(S_OK, now) && !recovery.active(), "Partial success did not clear the failure");
    const auto partialAt = now;
    require(firstAction(recovery, now, 4000, Action::kick) == partialAt + 4000, "Partial success reset the backoff");
    require(recovery.poll(now + 100, false) == Action::none, "Running upstream was retried");
    now += 200;
    const auto runningParkedAt = now;
    require(firstAction(recovery, now, 2000, Action::kick) == runningParkedAt + 2000, "Backoff did not reset after upstream ran");

    std::cout << "Output recovery: exclusive lock " << probes.size() << " probes in 60 s, " << kicks
              << " reinit; INTERNAL and unreported-park backoff 2/4/8/16/30/30 s\n";
}
}
