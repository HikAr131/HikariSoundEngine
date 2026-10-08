// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "startup_diagnostics.h"
#include "hikari_upstream_hooks.h"
#include <cstdio>
#include <exception>
#include <system_error>

namespace hikari {
namespace {
thread_local StartupDiagnostics* activeEnumerationDiagnostics = nullptr;
struct StepText { StartupStep step; const char* stage; const char* code; };
constexpr StepText steps[] = {
    {StartupStep::ownership, "ownership", "INSTANCE_OWNERSHIP_FAILED"},
    {StartupStep::stopEvent, "stop_event", "STOP_EVENT_FAILED"},
    {StartupStep::savedState, "saved_state", "SAVED_STATE_FAILED"},
    {StartupStep::pipe, "pipe", "PIPE_SETUP_FAILED"},
    {StartupStep::stateWrite, "state_write", "STATE_WRITE_FAILED"},
    {StartupStep::guardianEvent, "guardian", "GUARD_EVENT_FAILED"},
    {StartupStep::guardianLaunch, "guardian", "GUARD_LAUNCH_FAILED"},
    {StartupStep::guardianWait, "guardian", "GUARD_HANDSHAKE_FAILED"},
    {StartupStep::notificationsCreate, "notifications", "NOTIFICATION_CREATE_FAILED"},
    {StartupStep::notificationsRegister, "notifications", "NOTIFICATION_REGISTER_FAILED"},
    {StartupStep::outputRefresh, "output", "OUTPUT_ENUMERATION_FAILED"},
    {StartupStep::outputEndpoint, "output", "OUTPUT_ENDPOINT_UNAVAILABLE"},
    {StartupStep::outputVolume, "output", "OUTPUT_VOLUME_UNAVAILABLE"},
    {StartupStep::recoveryRestore, "recovery", "RECOVERY_RESTORE_FAILED"},
    {StartupStep::recoverySnapshot, "recovery", "RECOVERY_SNAPSHOT_FAILED"},
    {StartupStep::recoveryWrite, "recovery", "RECOVERY_COMMIT_FAILED"},
    {StartupStep::dspCreate, "dsp", "DSP_CREATE_FAILED"},
    {StartupStep::dspApply, "dsp", "DSP_PARAMETERS_FAILED"},
    {StartupStep::dspFormat, "dsp", "DSP_FORMAT_FAILED"},
    {StartupStep::audioInitialize, "audio", "AUDIO_INITIALIZE_FAILED"},
    {StartupStep::outputSelection, "output", "OUTPUT_SELECTION_FAILED"},
    {StartupStep::bufferInitialize, "buffer", "BUFFER_INITIALIZE_FAILED"},
    {StartupStep::sessionClass, "session", "SESSION_CLASS_FAILED"},
    {StartupStep::sessionWindow, "session", "SESSION_WINDOW_FAILED"},
    {StartupStep::enumerationCreate, "enumeration", "ENUMERATOR_CREATE_FAILED"},
    {StartupStep::enumerationList, "enumeration", "ENDPOINT_ENUMERATION_FAILED"},
    {StartupStep::enumerationCount, "enumeration", "ENDPOINT_COUNT_FAILED"},
    {StartupStep::enumerationDefault, "enumeration", "DEFAULT_ENDPOINT_FAILED"},
    {StartupStep::enumerationId, "enumeration", "ENDPOINT_ID_FAILED"},
    {StartupStep::enumerationItem, "enumeration", "ENDPOINT_ITEM_FAILED"},
    {StartupStep::enumerationProperties, "enumeration", "ENDPOINT_PROPERTIES_FAILED"},
    {StartupStep::enumerationFormat, "enumeration", "ENDPOINT_FORMAT_FAILED"},
    {StartupStep::enumerationState, "enumeration", "ENDPOINT_STATE_FAILED"},
    {StartupStep::enumerationClassification, "enumeration", "ENDPOINT_CLASSIFICATION_FAILED"}
};
}
std::string startupFailureLine(StartupStep step, std::uint32_t nativeCode) {
    for (const auto& text : steps) if (text.step == step) {
        char number[11]{};
        std::snprintf(number, sizeof(number), "0x%08X", static_cast<unsigned>(nativeCode));
        return std::string("Startup failure ") + text.stage + " " + text.code + " " + number;
    }
    return {};
}
void StartupDiagnostics::reportCurrentException() noexcept {
    std::uint32_t nativeCode = 0;
    try {
        const auto failure = std::current_exception();
        if (!failure) return;
        std::rethrow_exception(failure);
    } catch (const StartupNativeError& error) { nativeCode = error.nativeCode(); }
    catch (const std::system_error& error) { nativeCode = static_cast<std::uint32_t>(error.code().value()); }
    catch (...) {}
    try {
        const bool upstreamFailure = nativeCode == 0 && enumerationFailed_ &&
            (step_ == StartupStep::audioInitialize || step_ == StartupStep::outputSelection);
        const auto line = startupFailureLine(upstreamFailure ? enumerationStep_ : step_, upstreamFailure ? enumerationCode_ : nativeCode);
        if (!line.empty() && write_) write_(line);
    } catch (...) {}
}
StartupEnumerationScope::StartupEnumerationScope(StartupDiagnostics& diagnostics) noexcept : previous_(activeEnumerationDiagnostics) {
    activeEnumerationDiagnostics = &diagnostics;
}
StartupEnumerationScope::~StartupEnumerationScope() { activeEnumerationDiagnostics = previous_; }
}
void hikariBeginEnumeration() noexcept {
    if (hikari::activeEnumerationDiagnostics) hikari::activeEnumerationDiagnostics->beginEnumeration();
}
void hikariOnEnumerationFailure(int step, HRESULT result) noexcept {
    if (!hikari::activeEnumerationDiagnostics) return;
    using hikari::StartupStep;
    constexpr StartupStep steps[] = {StartupStep::enumerationCreate, StartupStep::enumerationList,
        StartupStep::enumerationCount, StartupStep::enumerationDefault, StartupStep::enumerationId,
        StartupStep::enumerationItem, StartupStep::enumerationProperties, StartupStep::enumerationFormat,
        StartupStep::enumerationState, StartupStep::enumerationClassification};
    if (step < 1 || step > static_cast<int>(sizeof(steps) / sizeof(*steps))) return;
    hikari::activeEnumerationDiagnostics->enumerationFailure(steps[step - 1], static_cast<std::uint32_t>(result));
}
