// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace hikari {
enum class StartupStep {
    ownership, stopEvent, savedState, pipe, stateWrite,
    guardianEvent, guardianLaunch, guardianWait,
    notificationsCreate, notificationsRegister,
    outputRefresh, outputEndpoint, outputVolume,
    recoveryRestore, recoverySnapshot, recoveryWrite,
    dspCreate, dspApply, dspFormat, audioInitialize,
    outputSelection, bufferInitialize, sessionClass, sessionWindow,
    enumerationCreate, enumerationList, enumerationCount, enumerationDefault,
    enumerationId, enumerationItem, enumerationProperties, enumerationFormat,
    enumerationState, enumerationClassification
};
class StartupNativeError final : public std::runtime_error {
public:
    StartupNativeError(const char* message, std::uint32_t code) : std::runtime_error(message), code_(code) {}
    std::uint32_t nativeCode() const noexcept { return code_; }
private:
    std::uint32_t code_;
};
std::string startupFailureLine(StartupStep step, std::uint32_t nativeCode);
class StartupDiagnostics {
public:
    explicit StartupDiagnostics(std::function<void(const std::string&)> write) : write_(std::move(write)) {}
    void step(StartupStep value) noexcept { step_ = value; }
    void complete() noexcept { complete_ = true; }
    void beginEnumeration() noexcept { enumerationFailed_ = false; }
    void enumerationFailure(StartupStep step, std::uint32_t code) noexcept {
        enumerationStep_ = step; enumerationCode_ = code; enumerationFailed_ = true;
    }
    void report() noexcept { if (!complete_) reportCurrentException(); }
    template <class Operation> decltype(auto) run(Operation&& operation) {
        try { return std::forward<Operation>(operation)(); }
        catch (...) { report(); throw; }
    }
private:
    void reportCurrentException() noexcept;
    std::function<void(const std::string&)> write_;
    StartupStep step_ = StartupStep::ownership;
    bool complete_ = false;
    StartupStep enumerationStep_ = StartupStep::enumerationCreate;
    std::uint32_t enumerationCode_ = 0;
    bool enumerationFailed_ = false;
};
class StartupEnumerationScope {
public:
    explicit StartupEnumerationScope(StartupDiagnostics&) noexcept;
    ~StartupEnumerationScope();
    StartupEnumerationScope(const StartupEnumerationScope&) = delete;
    StartupEnumerationScope& operator=(const StartupEnumerationScope&) = delete;
private:
    StartupDiagnostics* previous_;
};
void runStartupDiagnosticsTests();
}
