// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include "runtime.h"
#include "security.h"
#include "audio_platform.h"
#include "audio_format.h"
#include "audio_identity.h"
#include "pipe_server.h"
#include "parametric_eq.h"
#include "graphic_eq.h"
#include "dsp_adapter.h"
#include "storage.h"
#include "measure.h"
#include "parameter_json.h"
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <stdexcept>

void runProtocolTests();
namespace hikari { void runLifecycleTests(); void runStorageTests(); void runHostTests(); }
static void printJson(const hikari::Json& value) {
    auto text = value.stringify() + "\n"; DWORD written;
    auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE) WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}
static LONG WINAPI exceptionFilter(EXCEPTION_POINTERS*) {
    // The guardian owns abnormal-exit restoration; avoid invoking unsafe COM from a corrupted process.
    return EXCEPTION_EXECUTE_HANDLER;
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(exceptionFilter);
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { printJson(hikari::Json::object({{"ok", false}, {"code", "INTERNAL"}, {"message", "COM initialization failed"}})); return 1; }
    struct Uninitialize { ~Uninitialize() { CoUninitialize(); } } cleanup;
    try {
        int count = 0; LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!raw) throw std::runtime_error("Command-line parsing failed");
        std::vector<std::wstring> args;
        for (int i = 1; i < count; ++i) args.emplace_back(raw[i]);
        LocalFree(raw);
        std::wstring command = L"run";
        if (!args.empty() && (args.front().empty() || args.front()[0] != L'-' || args.front() == L"--version" || args.front() == L"--self-test" || args.front() == L"--probe-devices" || args.front() == L"--test-child")) { command = args.front(); args.erase(args.begin()); }
        hikari::RunOptions options;
        DWORD required = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
        if (required) { std::vector<wchar_t> appdata(required); GetEnvironmentVariableW(L"APPDATA", appdata.data(), required); options.dataDir = std::filesystem::path(appdata.data()) / "HikariSoundEngine"; }
        unsigned long watchedPid = 0; std::string creation, instance;
        std::filesystem::path parameterFile;
        unsigned measureRate = 48000;
        for (std::size_t i = 0; i < args.size(); ++i) {
            auto value = [&]() -> const std::wstring& { if (++i == args.size()) throw std::invalid_argument("Missing argument value"); return args[i]; };
            if (args[i] == L"--data-dir") options.dataDir = value();
            else if (args[i] == L"--capture-endpoint") options.captureId = value();
            else if (args[i] == L"--output-endpoint") options.outputId = value();
            else if (args[i] == L"--no-default-switch") options.noDefaultSwitch = true;
            else if (command == L"guard" && args[i] == L"--pid") { auto v = value(); std::size_t used; auto n = std::stoul(v, &used); if (used != v.size() || n == 0) throw std::invalid_argument("Invalid PID"); watchedPid = n; }
            else if (command == L"guard" && args[i] == L"--creation") creation = hikari::utf8(value());
            else if (command == L"guard" && args[i] == L"--instance") instance = hikari::utf8(value());
            else if (command == L"measure" && args[i] == L"--params") parameterFile = value();
            else if (command == L"measure" && args[i] == L"--rate") { auto v = value(); std::size_t used; auto n = std::stoul(v, &used); if (used != v.size() || n > 192000) throw std::invalid_argument("Invalid measurement rate"); measureRate = static_cast<unsigned>(n); }
            else throw std::invalid_argument("Unknown argument");
        }
        if (command == L"--version") { printJson(hikari::Json::object({{"ok", true}, {"name", "HikariSoundEngine"}, {"version", "1.0.0"}, {"protocol", 1}})); return 0; }
        if (command == L"--test-child") { Sleep(100); return 7; }
        if (command == L"--self-test") {
            auto test = [](const char* name, void (*run)()) { printJson(hikari::Json::object({{"test", name}, {"state", "running"}})); run(); };
            test("protocol", runProtocolTests); test("parametric", hikari::runEqTests); test("graphic", hikari::runGraphicTests);
            test("lifecycle", hikari::runLifecycleTests); test("storage", hikari::runStorageTests); test("host", hikari::runHostTests);
            test("audio-format", hikari::runAudioFormatTests); test("audio-identity", hikari::runAudioIdentityTests);
            test("dsp", hikari::runDspTests); test("measure", hikari::runMeasureTests);
            printJson(hikari::Json::object({{"ok", true}, {"selfTest", "passed"}, {"eqVectorPoints", 14336}, {"audioDevicesTouched", false}})); return 0;
        }
        if (command == L"--probe-devices") { printJson(hikari::probeDevices()); return 0; }
        if (command == L"measure") {
            if (parameterFile.empty() || !std::filesystem::is_regular_file(parameterFile) || std::filesystem::file_size(parameterFile) > 65536) throw std::invalid_argument("Measurement parameters file invalid");
            std::ifstream input(parameterFile, std::ios::binary);
            if (!input) throw std::invalid_argument("Measurement parameters read failed");
            std::string contents((std::istreambuf_iterator<char>(input)), {});
            auto parameters = hikari::parseParameters(hikari::Json::parse(contents));
            printJson(hikari::measureResponse(parameters, measureRate)); return 0;
        }
        if (command == L"status" || command == L"devices") {
            const auto pipeName = hikari::currentSessionPipeName();
            if (!WaitNamedPipeW(pipeName.c_str(), 1)) {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                    printJson(hikari::Json::object({{"ok", true}, {"running", false}})); return 0;
                }
            }
            try { printJson(hikari::PipeServer::requestOnce(hikari::Json::object({{"id", 1}, {"cmd", hikari::utf8(command)}}))); }
            catch (...) { printJson(hikari::Json::object({{"ok", false}, {"code", "INTERNAL"}, {"message", "Engine status could not be confirmed"}})); return 1; }
            return 0;
        }
        if (command == L"stop") {
            hikari::Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\Hikari1U.SoundEngine.Stop"));
            if (!event.get()) { printJson(hikari::Json::object({{"ok", true}, {"running", false}})); return 0; }
            hikari::Handle owner(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Local\\Hikari1U.SoundEngine.Run"));
            if (!SetEvent(event.get())) throw std::runtime_error("Stop signal failed");
            DWORD wait = owner.get() ? WaitForSingleObject(owner.get(), 5000) : WAIT_OBJECT_0;
            if (owner.get() && (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)) ReleaseMutex(owner.get());
            bool stopped = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
            printJson(hikari::Json::object({{"ok", stopped}, {"running", !stopped}})); return stopped ? 0 : 2;
        }
        if (options.dataDir.empty() || !options.dataDir.is_absolute()) throw std::invalid_argument("Absolute data directory required");
        if (command == L"guard") {
            if (!watchedPid) throw std::invalid_argument("Guard PID required");
            if (creation.empty() || instance.empty()) {
                // The standalone guard form obtains evidence from the engine-owned state file.
                hikari::Storage storage(options.dataDir);
                auto state = storage.read("state.json", false);
                instance = state.at("instance").asString();
                hikari::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, watchedPid));
                if (!process.get()) throw std::invalid_argument("Guard process unavailable");
                creation = hikari::processCreation(process.get());
            }
            return hikari::runGuard(options, watchedPid, creation, instance);
        }
        if (command != L"run") throw std::invalid_argument("Unknown command");
        return hikari::runEngine(options);
    } catch (const hikari::JsonError&) {
        printJson(hikari::Json::object({{"ok", false}, {"code", "BAD_REQUEST"}, {"message", "Invalid JSON parameters"}})); return 2;
    } catch (const std::invalid_argument&) {
        printJson(hikari::Json::object({{"ok", false}, {"code", "BAD_REQUEST"}, {"message", "Invalid command-line arguments"}})); return 2;
    } catch (const std::exception& exception) {
        printJson(hikari::Json::object({{"ok", false}, {"code", "INTERNAL"}, {"message", exception.what()}})); return 1;
    }
}
