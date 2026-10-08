// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "startup_diagnostics.h"
#include "hikari_upstream_hooks.h"
#include <system_error>
#include <vector>

namespace hikari {
void runStartupDiagnosticsTests() {
    auto check = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    std::vector<std::string> rows;
    StartupDiagnostics diagnostics([&](const std::string& line) { rows.push_back(line); });
    bool rethrown = false;
    try {
        diagnostics.run([&] {
            diagnostics.step(StartupStep::guardianLaunch);
            throw StartupNativeError("private source path and device identity", 5);
        });
    } catch (const StartupNativeError& error) { rethrown = error.nativeCode() == 5; }
    check(rethrown, "Startup diagnostics changed the original exception");
    check(rows.size() == 1, "Startup failure record missing");
    check(rows.front() == "Startup failure guardian GUARD_LAUNCH_FAILED 0x00000005", "Startup failure leaked text or lost native code");
    check(startupFailureLine(StartupStep::notificationsCreate, 0x8889000aU) ==
        "Startup failure notifications NOTIFICATION_CREATE_FAILED 0x8889000A", "HRESULT formatting changed");
    check(startupFailureLine(static_cast<StartupStep>(999), 5).empty(), "Unknown startup step was emitted");
    rows.clear();
    try {
        diagnostics.run([&] { diagnostics.step(StartupStep::pipe); throw std::system_error(32, std::system_category(), "private path"); });
    } catch (const std::system_error&) {}
    check(rows.size() == 1 && rows.front() == "Startup failure pipe PIPE_SETUP_FAILED 0x00000020", "System error code was lost");
    rows.clear();
    try {
        diagnostics.run([&] { diagnostics.step(StartupStep::outputSelection); throw std::runtime_error("device identity"); });
    } catch (const std::runtime_error&) {}
    check(rows.size() == 1 && rows.front() == "Startup failure output OUTPUT_SELECTION_FAILED 0x00000000", "Unknown exception text was emitted");
    StartupDiagnostics brokenWriter([](const std::string&) { throw std::runtime_error("write failed"); });
    rethrown = false;
    try { brokenWriter.run([] { throw StartupNativeError("original", 5); }); }
    catch (const StartupNativeError&) { rethrown = true; }
    check(rethrown, "Diagnostic write failure replaced the startup exception");
    rows.clear();
    check(diagnostics.run([] { return 7; }) == 7 && rows.empty(), "Successful startup emitted a failure");
    {
        StartupEnumerationScope scope(diagnostics);
        hikariOnEnumerationFailure(7, E_FAIL);
        hikariBeginEnumeration();
        try { diagnostics.run([&] { diagnostics.step(StartupStep::outputSelection); throw std::runtime_error("output"); }); }
        catch (const std::runtime_error&) {}
        check(rows.size() == 1 && rows.front() == "Startup failure output OUTPUT_SELECTION_FAILED 0x00000000", "Retried enumeration kept a stale failure");
        rows.clear();
        hikariOnEnumerationFailure(999, E_FAIL);
        try { diagnostics.run([&] { diagnostics.step(StartupStep::audioInitialize); throw std::runtime_error("audio"); }); }
        catch (const std::runtime_error&) {}
        check(rows.size() == 1 && rows.front() == "Startup failure audio AUDIO_INITIALIZE_FAILED 0x00000000", "Unknown enumeration hook was accepted");
        rows.clear();
    }
    hikariOnEnumerationFailure(7, E_FAIL);
    try { diagnostics.run([] { throw std::runtime_error("audio"); }); }
    catch (const std::runtime_error&) {}
    check(rows.size() == 1 && rows.front() == "Startup failure audio AUDIO_INITIALIZE_FAILED 0x00000000", "Enumeration scope remained active after destruction");
    rows.clear();
    diagnostics.complete();
    try { diagnostics.run([] { throw std::runtime_error("runtime failure"); }); }
    catch (const std::runtime_error&) {}
    check(rows.empty(), "Runtime failure was classified as startup failure");
}
}
