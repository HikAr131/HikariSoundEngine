// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "lifecycle.h"
#include "audio_platform.h"
#include "json.h"
#include "runtime.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <stdexcept>
#include <type_traits>

namespace hikari {
// 282/3, 282/4: the constructor's fallback role records stored eConsole as false, so every start that found the
// virtual card already holding the default threw at the recovery snapshot. An enum must become a number first.
static_assert(!std::is_constructible_v<Json, ERole>, "an ERole must be converted to a number before it becomes Json");
static_assert(std::is_constructible_v<Json, unsigned>, "role numbers stay storable");
void runLifecycleTests() {
    auto check = [](bool value) { if (!value) throw std::runtime_error("Lifecycle test failed"); };
    PauseReasons pauses;
    check(pauses.set(PauseReasons::remote, true)); check(pauses.set(PauseReasons::locked, true));
    check(pauses.set(PauseReasons::locked, false)); check(pauses.set(PauseReasons::power, true));
    check(pauses.set(PauseReasons::remote, false)); check(!pauses.set(PauseReasons::power, false));
    unsigned defaultChanges = 0; std::wstring previousDefault;
    for (const auto* id : {L"a", L"a", L"virtual", L"virtual", L"a", L"a", L"virtual", L"a"})
        if (acceptDefaultChange(id, L"virtual", previousDefault)) ++defaultChanges;
    check(defaultChanges == 3);
    DefaultNotifications notifications;
    notifications.ownWrite(0, L"physical", 100);
    notifications.ownWrite(1, L"physical", 101);
    check(!notifications.accept(0, L"physical", L"virtual", 99));
    check(!notifications.accept(1, L"physical", L"virtual", 102));
    check(!notifications.accept(0, L"virtual", L"virtual", 200));
    check(notifications.accept(0, L"physical", L"virtual", 201));
    // The persistent callback can deliver another role or duplicate across a later tick.
    check(!notifications.accept(1, L"physical", L"virtual", 301));
    check(!notifications.accept(0, L"physical", L"virtual", 401));
    check(!notifications.accept(0, L"virtual", L"virtual", 500));
    check(notifications.accept(0, L"physical", L"virtual", 501));
    DefaultNotifications expired;
    expired.ownWrite(0, L"physical", 100);
    check(expired.accept(0, L"physical", L"virtual", 5101));
    DefaultNotifications roleSpecific;
    roleSpecific.ownWrite(1, L"physical", 100);
    check(roleSpecific.accept(0, L"physical", L"virtual", 101));
    check(!roleSpecific.accept(1, L"physical", L"virtual", 102));
    DefaultNotifications paused;
    paused.ownWrite(0, L"physical", 100);
    // Classification uses callback time, preserving ownership while a paused queue is delayed.
    check(!paused.accept(0, L"physical", L"virtual", 101));
    DefaultConflict conflict;
    check(!conflict.observe(100)); check(!conflict.observe(1000)); check(conflict.observe(2000));
    DefaultConflict aged;
    check(!aged.observe(1)); check(!aged.observe(10002)); check(!aged.observe(11000)); check(aged.observe(12000));
    for (bool clean : {false, true}) for (bool newer : {false, true}) for (bool debug : {false, true})
        check(shouldGuardRestore(clean, newer, debug) == (!clean && !newer));
    check(guardDecision(false, true, false, true, false, true) == GuardDecision::wait);
    check(guardDecision(false, true, true, true, false, true) == GuardDecision::retire);
    check(guardDecision(true, true, false, true, false, true) == GuardDecision::wait);
    check(guardDecision(true, true, false, false, false, true) == GuardDecision::restore);
    check(guardDecision(true, false, false, false, false, true) == GuardDecision::restore);
    check(guardDecision(true, false, false, false, true, true) == GuardDecision::done);
    check(guardDecision(true, false, false, false, false, false) == GuardDecision::done);
    check(canDispatchMutation(false, false)); check(!canDispatchMutation(true, false));
    check(!canDispatchMutation(false, true)); check(!canDispatchMutation(true, true));
    check(canDrainCommandQueue(false, false, false));
    check(!canDrainCommandQueue(true, false, false));
    check(!canDrainCommandQueue(false, true, false));
    check(!canDrainCommandQueue(false, false, true));
    check(requiresModeRecovery(true, false, true)); check(requiresModeRecovery(true, true, false));
    check(!requiresModeRecovery(true, false, false)); check(!requiresModeRecovery(true, true, true));
    check(!requiresModeRecovery(false, false, true)); check(!requiresModeRecovery(false, true, false));
    bool inheritedPending = true;
    unsigned restoreCalls = 0;
    check(completePendingRecovery(inheritedPending, [&] { ++restoreCalls; return true; }));
    check(!inheritedPending && restoreCalls == 1);
    check(completePendingRecovery(inheritedPending, [&] { ++restoreCalls; return true; }));
    check(restoreCalls == 1);
    bool failedPausedPending = true;
    check(!completePendingRecovery(failedPausedPending, [] { return false; }));
    check(failedPausedPending && guardDecision(true, false, false, false, false, failedPausedPending) == GuardDecision::restore);
    struct EndpointFixture {
        std::wstring id; bool virtualDevice, consoleDefault, multimediaDefault;
        float volume; bool muted, volumeKnown;
    };
    const std::vector<RecoveryRecord> oldRoles{{0, L"a", 0.3f, false}, {1, L"a", 0.3f, false}};
    const auto resumed = freshRecoverySnapshot(std::vector<EndpointFixture>{{L"a", false, true, true, 0.8f, true, true}}, L"a", oldRoles);
    check(resumed.roles.size() == 2 && resumed.outputs.size() == 1);
    check(std::abs(resumed.roles[0].volume - 0.8f) < 0.00001f && resumed.roles[0].muted);
    check(std::abs(resumed.outputs[0].volume - 0.8f) < 0.00001f && resumed.outputs[0].muted);
    const auto changedDefault = freshRecoverySnapshot(std::vector<EndpointFixture>{
        {L"a", false, false, false, 0.6f, false, true}, {L"b", false, true, true, 0.8f, false, true}}, L"a", oldRoles);
    check(changedDefault.roles[0].id == L"b" && changedDefault.roles[1].id == L"b");
    check(changedDefault.outputs[0].id == L"a" && std::abs(changedDefault.outputs[0].volume - 0.6f) < 0.00001f);
    const auto hinted = freshRecoverySnapshot(std::vector<EndpointFixture>{
        {L"virtual", true, true, true, 1.0f, false, true}, {L"a", false, false, false, 0.8f, false, true}}, L"a", oldRoles);
    check(hinted.roles[0].id == L"a" && std::abs(hinted.roles[0].volume - 0.8f) < 0.00001f);
    check(launchHintFresh(1000, 61000)); check(!launchHintFresh(1000, 61001)); check(!launchHintFresh(1001, 1000));
    // The constructor's records for a start that found the virtual card holding both defaults: both media roles, as
    // numbers, to the device known from before; the snapshot then records both roles from them.
    Endpoint speakers; speakers.id = L"speakers"; speakers.volume = 0.4f; speakers.muted = true; speakers.volumeKnown = true;
    std::vector<RecoveryRecord> fallback;
    for (const auto& record : fallbackRoleRecords(speakers)) {
        const auto saved = Json::parse(record.stringify());
        fallback.push_back({static_cast<unsigned>(saved.at("role").asNumber()), wide(saved.at("id").asString()),
            static_cast<float>(saved.at("volume").asNumber()), saved.at("muted").asBool()});
    }
    check(fallback.size() == 2 && fallback[0].role == eConsole && fallback[1].role == eMultimedia && fallback[1].muted);
    const auto fromFallback = freshRecoverySnapshot(std::vector<EndpointFixture>{
        {L"virtual", true, true, true, 1.0f, false, true}, {L"speakers", false, false, false, 0.4f, true, true}}, L"speakers", fallback);
    check(fromFallback.roles.size() == 2 && fromFallback.roles[0].id == L"speakers" && fromFallback.roles[1].id == L"speakers");
    check(chooseOutput({L"a", L"b"}, {L"gone", L"b"}, {}) == L"b");
    check(chooseOutput({L"a"}, {L"b", L"a"}, L"b").empty());
    check(chooseOutput({L"a", L"b"}, {L"b", L"a"}, L"a") == L"a");
    check(chooseOutput({}, {L"b"}, {}).empty());
}
}
