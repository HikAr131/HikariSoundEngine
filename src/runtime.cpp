// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "runtime.h"
#include "audio_platform.h"
#include "output_recovery.h"
#include "security.h"
#include "storage.h"
#include "lifecycle.h"
#include "parameter_json.h"
#include "pipe_server.h"
#include "dsp_adapter.h"
#include "version.h"
#include "startup_diagnostics.h"
#include "AudioPassthru.h"
#include <windows.h>
#include <wtsapi32.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <future>
#include <functional>
#include <deque>
#include <memory>
#include <algorithm>
#include <thread>
#include <stdexcept>
#include <cmath>

namespace {
std::atomic<bool> allowUpstreamSwitch{false};
std::atomic<bool> endpointChanged{false};
std::atomic<bool> persistentNotifications{false};
std::mutex defaultEventsMutex;
struct DefaultEvent { unsigned role; std::wstring id; ULONGLONG time; };
std::deque<DefaultEvent> defaultEvents;
hikari::DefaultNotifications defaultNotifications;
constexpr wchar_t runMutexName[] = L"Local\\Hikari1U.SoundEngine.Run";
constexpr wchar_t stopEventName[] = L"Local\\Hikari1U.SoundEngine.Stop";
hikari::Storage* activeLog = nullptr;
std::mutex activeLogMutex;
std::mutex preferredOutputMutex;
std::wstring preferredOutput;
}
bool hikariGetPreferredOutput(wchar_t* buffer, int capacity) {
    std::lock_guard<std::mutex> lock(preferredOutputMutex);
    if (preferredOutput.empty() || capacity <= 0 || preferredOutput.size() >= static_cast<std::size_t>(capacity)) return false;
    std::copy(preferredOutput.begin(), preferredOutput.end(), buffer); buffer[preferredOutput.size()] = 0; return true;
}
extern "C" void hikariLogUpstreamError(const wchar_t* message) {
    try { std::lock_guard<std::mutex> lock(activeLogMutex); if (activeLog && message) activeLog->log(hikari::utf8(message)); } catch (...) {}
}
bool hikariAllowDefaultSwitch(const wchar_t*) { return allowUpstreamSwitch.load(); }
static void enqueueDefaultDeviceChanged(int flow, int role, const wchar_t* id) {
    if (flow != eRender || (role != eConsole && role != eMultimedia) || !id) return;
    std::lock_guard<std::mutex> lock(defaultEventsMutex);
    if (defaultEvents.size() < 32) defaultEvents.push_back({static_cast<unsigned>(role), id, GetTickCount64()});
    endpointChanged.store(true);
}
void hikariOnDefaultDeviceChanged(int flow, int role, const wchar_t* id) {
    if (!persistentNotifications.load()) enqueueDefaultDeviceChanged(flow, role, id);
}
void hikariOnOwnDefaultWrite(unsigned role, const wchar_t* id) {
    if (!id || (role != eConsole && role != eMultimedia)) return;
    std::lock_guard<std::mutex> lock(defaultEventsMutex);
    defaultNotifications.ownWrite(role, id, GetTickCount64());
}

namespace hikari {
using Microsoft::WRL::ComPtr;
static Json endpointJson(const Endpoint& e) {
    return Json::object({{"id", utf8(e.id)}, {"name", e.name}, {"isVirtual", e.virtualDevice},
        {"isDefault", e.consoleDefault || e.multimediaDefault}, {"channels", e.channels}, {"sampleRate", e.sampleRate}});
}
Json probeDevices() {
    Json::Array list;
    for (const auto& e : enumerateEndpoints()) list.push_back(endpointJson(e));
    return Json::object({{"ok", true}, {"devices", list}, {"consoleDefault", utf8(defaultEndpoint(eConsole))}, {"multimediaDefault", utf8(defaultEndpoint(eMultimedia))}, {"communicationsDefault", utf8(defaultEndpoint(eCommunications))}});
}
static Json error(const char* code, const char* message) {
    return Json::object({{"ok", false}, {"code", code}, {"message", message}});
}
static Json::Array validatedRecoveryArray(const Json& value, bool roles) {
    const auto& items = value.asArray();
    if (items.size() > (roles ? 2U : 128U)) throw JsonError("Too many recovery records");
    for (const auto& item : items) {
        const auto& id = item.at("id").asString();
        double volume = item.at("volume").asNumber();
        item.at("muted").asBool();
        if (id.empty() || id.size() > 4096 || !std::isfinite(volume) || volume < 0 || volume > 1) throw JsonError("Invalid recovery record");
        for (unsigned char c : id) if (c < 32 || c == 127) throw JsonError("Invalid recovery endpoint");
        if (roles) { double role = item.at("role").asNumber(); if (role != eConsole && role != eMultimedia) throw JsonError("Invalid recovery role"); }
    }
    return items;
}
static void outputLine(const Json& value) {
    auto line = value.stringify() + "\n"; DWORD written = 0;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle && handle != INVALID_HANDLE_VALUE) WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}
class Notifications final : public IMMNotificationClient {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) { *object = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK; }
        *object = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        // Default events are forwarded by this persistent callback even while the upstream object is absent.
        enqueueDefaultDeviceChanged(flow, role, id); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { endpointChanged = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { endpointChanged = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { endpointChanged = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { endpointChanged = true; return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
};
static bool restoreRecorded(const Json& state) {
    if (!state.isObject() || !state.contains("restore")) return false;
    const bool noDefaultSwitch = state.contains("noDefaultSwitch") && state.at("noDefaultSwitch").asBool();
    const auto records = validatedRecoveryArray(state.at("restore"), true);
    bool restored = true;
    unsigned attempted = 0;
    auto active = enumerateEndpoints();
    bool volumesRestored = true;
    if (state.contains("outputVolumes")) for (const auto& saved : validatedRecoveryArray(state.at("outputVolumes"), false)) {
        const auto id = wide(saved.at("id").asString());
        const auto e = std::find_if(active.begin(), active.end(), [&](const Endpoint& item) { return item.id == id && !item.virtualDevice; });
        if (e != active.end()) volumesRestored = restoreVolume(id, static_cast<float>(saved.at("volume").asNumber()), saved.at("muted").asBool()) && volumesRestored;
    }
    for (const auto& record : records) {
        auto id = wide(record.at("id").asString());
        auto found = std::find_if(active.begin(), active.end(), [&](const Endpoint& e) { return e.id == id && !e.virtualDevice; });
        auto role = record.at("role").asNumber();
        if (role != eConsole && role != eMultimedia) continue;
        if (noDefaultSwitch) {
            if (found != active.end()) volumesRestored = restoreVolume(id, static_cast<float>(record.at("volume").asNumber()), record.at("muted").asBool()) && volumesRestored;
            continue;
        }
        ++attempted;
        if (found == active.end()) {
            bool fallback = false;
            if (state.contains("outputId")) {
                const auto outputId = wide(state.at("outputId").asString());
                const auto output = std::find_if(active.begin(), active.end(), [&](const Endpoint& e) { return e.id == outputId && !e.virtualDevice; });
                if (output != active.end()) fallback = setDefaultEndpoint(outputId, static_cast<unsigned>(role));
            }
            restored = restored && fallback;
            continue;
        }
        bool ok = restoreVolume(id, static_cast<float>(record.at("volume").asNumber()), record.at("muted").asBool());
        ok = setDefaultEndpoint(id, static_cast<unsigned>(role)) && ok;
        restored = restored && ok;
    }
    // The chosen output is the fallback if the original endpoints have disappeared.
    if (!noDefaultSwitch && !attempted && state.contains("outputId")) {
        auto id = wide(state.at("outputId").asString());
        auto found = std::find_if(active.begin(), active.end(), [&](const Endpoint& e) { return e.id == id && !e.virtualDevice; });
        if (found != active.end()) { restored = setDefaultEndpoint(id, eConsole); ++attempted; }
    }
    return (noDefaultSwitch || (restored && attempted > 0)) && volumesRestored;
}
class Engine {
public:
    explicit Engine(const RunOptions& options, std::vector<Endpoint> endpoints, StartupDiagnostics& diagnostics) : options_(options), storage_(options.dataDir), endpoints_(std::move(endpoints)) {
        diagnostics.step(StartupStep::savedState);
        instance_ = std::to_string(GetCurrentProcessId()) + "-" + processCreation(GetCurrentProcess());
        paused_ = GetSystemMetrics(SM_REMOTESESSION) != 0;
        auto previous = storage_.read("state.json");
        if (!previous.isNull()) {
            if (!previous.isObject()) throw std::runtime_error("Invalid saved state");
            if (previous.contains("applied")) parameters_ = restoredParameters(previous.at("applied"));
            if (previous.contains("mode")) mode_ = previous.at("mode").asString();
            if (mode_ != "follow" && mode_ != "fixed") throw std::runtime_error("Invalid saved output mode");
            if (previous.contains("fixedId")) fixedId_ = wide(previous.at("fixedId").asString());
            if (previous.contains("bufferMs")) { double ms = previous.at("bufferMs").asNumber(); if (ms < 10 || ms > 100 || std::floor(ms) != ms) throw std::runtime_error("Invalid saved buffer"); bufferMs_ = static_cast<int>(ms); }
            if (previous.contains("preferred")) for (const auto& id : previous.at("preferred").asArray()) { if (preferred_.size() >= 64) break; preferred_.push_back(wide(id.asString())); }
            const bool previousPending = previous.contains("cleanExit") && !previous.at("cleanExit").asBool() &&
                (!previous.contains("recoveryPending") || previous.at("recoveryPending").asBool());
            if (previousPending && requiresModeRecovery(previousPending, previous.at("noDefaultSwitch").asBool(), options_.noDefaultSwitch)) {
                // Finish ownership under the previous mode before recording a fresh recovery snapshot.
                diagnostics.step(StartupStep::recoveryRestore);
                if (!restoreRecorded(previous)) throw std::runtime_error("Previous-mode recovery failed");
                previous["cleanExit"] = true; previous["recoveryPending"] = false;
                diagnostics.step(StartupStep::stateWrite);
                storage_.writeState(previous);
                diagnostics.step(StartupStep::outputRefresh);
                endpoints_ = enumerateEndpoints();
            }
            diagnostics.step(StartupStep::savedState);
            if (previous.contains("cleanExit") && !previous.at("cleanExit").asBool() &&
                (!previous.contains("recoveryPending") || previous.at("recoveryPending").asBool())) {
                restore_ = validatedRecoveryArray(previous.at("restore"), true);
                if (previous.contains("outputVolumes")) outputVolumes_ = validatedRecoveryArray(previous.at("outputVolumes"), false);
                recoveryPending_ = true;
            }
        }
        if (!options_.outputId.empty()) { fixedId_ = options_.outputId; mode_ = "fixed"; }
        diagnostics.step(StartupStep::outputVolume);
        for (const auto& e : endpoints_) {
            if (e.virtualDevice) virtualId_ = e.id;
            if (!e.virtualDevice && (e.consoleDefault || e.multimediaDefault)) {
                if (!e.volumeKnown) throw std::runtime_error("Original endpoint volume could not be read");
                remember(e.id);
                auto saveRole = [&](unsigned role) {
                    for (const auto& saved : restore_) if (saved.at("role").asNumber() == role) return;
                    restore_.push_back(Json::object({{"role", role}, {"id", utf8(e.id)}, {"volume", e.volume}, {"muted", e.muted}}));
                };
                if (e.consoleDefault) saveRole(eConsole);
                if (e.multimediaDefault) saveRole(eMultimedia);
            }
        }
        diagnostics.step(StartupStep::savedState);
        auto hint = storage_.read("launch-hint.json");
        if (!hint.isNull()) {
            if (!hint.isObject()) throw std::runtime_error("Invalid launch hint");
            // Canonical v1 hint: writtenAtMs, outputId. Ignore stale hints.
            if (hint.contains("writtenAtMs") && hint.contains("outputId")) {
                double stamp = hint.at("writtenAtMs").asNumber();
                if (std::isfinite(stamp) && stamp >= 0 && stamp <= static_cast<double>(unixMillis()) && launchHintFresh(static_cast<unsigned long long>(stamp), unixMillis())) {
                    auto id = wide(hint.at("outputId").asString());
                    auto found = findEndpoint(id);
                    if (found && !found->virtualDevice && found->volumeKnown) {
                        remember(id);
                        if (restore_.empty()) restore_.push_back(Json::object({{"role", eConsole}, {"id", utf8(id)}, {"volume", found->volume}, {"muted", found->muted}}));
                    }
                }
            }
        }
        selectOutput();
        if (restore_.empty() && !outputId_.empty()) {
            diagnostics.step(StartupStep::outputVolume);
            auto e = findEndpoint(outputId_);
            if (!e->volumeKnown) throw std::runtime_error("Original output volume could not be read");
            restore_.push_back(Json::object({{"role", eConsole}, {"id", utf8(e->id)}, {"volume", e->volume}, {"muted", e->muted}}));
        }
    }
    ~Engine() { shutdown(); }
    void initialize(StartupDiagnostics& diagnostics) {
        { std::lock_guard<std::mutex> lock(activeLogMutex); activeLog = &storage_; }
        diagnostics.step(StartupStep::stateWrite);
        persist(false);
        startGuardian(diagnostics);
        guardianReady_ = true;
        diagnostics.step(StartupStep::stateWrite);
        persist(false);
        diagnostics.step(StartupStep::notificationsCreate);
        HRESULT notificationResult = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
        if (FAILED(notificationResult)) throw StartupNativeError("Audio notification setup failed", static_cast<std::uint32_t>(notificationResult));
        notifications_.Attach(new Notifications());
        diagnostics.step(StartupStep::notificationsRegister);
        notificationResult = enumerator_->RegisterEndpointNotificationCallback(notifications_.Get());
        if (FAILED(notificationResult)) throw StartupNativeError("Audio notification setup failed", static_cast<std::uint32_t>(notificationResult));
        registered_ = true;
        persistentNotifications = true;
        startAudio(&diagnostics);
        storage_.log("Engine initialized");
    }
    Json dispatch(const Json& request) {
        const auto cmd = request.at("cmd").asString();
        const bool mutating = cmd == "apply" || cmd == "set-output" || cmd == "set-buffer" || cmd == "quit";
        if (mutating && !canDispatchMutation(shutdown_, quitting_))
            return withId(error("INTERNAL", "Engine is stopping"), request);
        const auto previousParameters = parameters_;
        const auto previousMode = mode_;
        const auto previousFixed = fixedId_;
        const auto previousPreferred = preferred_;
        const auto previousOutput = outputId_;
        const auto previousBuffer = bufferMs_;
        bool mutationStarted = false;
        auto rollback = [&] {
            parameters_ = previousParameters; mode_ = previousMode; fixedId_ = previousFixed;
            preferred_ = previousPreferred; outputId_ = previousOutput; bufferMs_ = previousBuffer;
            operationFailed();
        };
        Json response;
        try {
            if (cmd == "hello") response = Json::object({{"ok", true}, {"name", "HikariSoundEngine"}, {"version", HIKARI_VERSION_STRING}, {"protocol", HIKARI_PROTOCOL_VERSION}, {"pid", GetCurrentProcessId()}});
            else if (cmd == "status" || cmd == "subscribe") { response = status(); }
            else if (cmd == "devices") { response = devices(); }
            else if (cmd == "apply") {
                auto p = parseParameters(request.at("params"));
                // Identical parameters neither touch the audio path nor rewrite the state file.
                if (!sameParameters(p, parameters_)) {
                    mutationStarted = true;
                    if (dsp_) dsp_->apply(p);
                    parameters_ = std::move(p); persist(false);
                }
                response = Json::object({{"ok", true}, {"applied", parametersJson(parameters_)}});
            } else if (cmd == "set-output") {
                auto mode = request.at("mode").asString(); std::wstring fixed;
                if (mode == "fixed") { fixed = wide(request.at("deviceId").asString()); auto e = findEndpoint(fixed); if (!e || e->virtualDevice) return withId(error("NO_OUTPUT_DEVICE", "Output endpoint is unavailable"), request); }
                if (mode != "fixed" && mode != "follow") throw JsonError("Invalid output mode");
                mutationStarted = true;
                mode_ = mode; fixedId_ = fixed; if (!fixed.empty()) remember(fixed);
                selectOutput(); stopAudio(); startAudio(); persist(false); response = status();
            } else if (cmd == "set-buffer") {
                const auto ms = request.at("ms").asNumber();
                if (!std::isfinite(ms)) throw JsonError("Invalid buffer duration");
                mutationStarted = true;
                bufferMs_ = static_cast<int>(std::clamp(ms, 10.0, 100.0));
                if (audio_) { stopAudio(); startAudio(); }
                ++reinitCount_; lastReinitReason_ = "buffer-change"; persist(false); response = status();
            } else if (cmd == "quit") { quitting_ = true; response = Json::object({{"ok", true}}); }
            else response = error("BAD_REQUEST", "Unknown command");
        } catch (const JsonError&) {
            if (mutationStarted) rollback();
            response = error("BAD_REQUEST", "Invalid request parameters");
        } catch (const std::exception&) {
            if (mutationStarted) rollback();
            response = error("INTERNAL", "Engine operation failed"); storage_.log("Operation failed");
        }
        return withId(response, request);
    }
    static Json withId(Json response, const Json& request) { if (auto id = request.get("id")) response["id"] = *id; return response; }
    Json status() const {
        Json output = Json();
        auto e = findEndpoint(outputId_);
        if (e) { output = endpointJson(*e); output["mode"] = mode_; }
        const bool virtualDefault = !virtualId_.empty() && defaultEndpoint(eConsole) == virtualId_;
        // Ready does not depend on audio playing: silent loopback delivers no frames at all.
        const bool ready = audio_ && outputReady(outputInitialized_, !audio_->isPlaybackDeviceAvailable(), virtualDefault, options_.noDefaultSwitch);
        const auto current = visibleEngineState(state_, audio_ != nullptr, paused_, outputRecovery_.active(), parameters_.bypass, processing_, ready);
        return Json::object({{"ok", true}, {"running", true}, {"state", current}, {"output", output},
            {"virtual", Json::object({{"present", !virtualId_.empty()}, {"isDefault", virtualDefault}})},
            {"bufferMs", bufferMs_}, {"stats", Json::object({{"underruns", Json()}, {"underrunMeasurementAvailable", false}, {"reinitCount", reinitCount_}, {"uptimeSec", (GetTickCount64() - started_) / 1000}, {"lastReinitReason", lastReinitReason_},
                {"applyLockMaxUs", dsp_ ? Json((dsp_->stats().applyLockMaxNanoseconds + 999) / 1000) : Json()}})},
            {"conflict", Json::object({{"detected", state_ == "yielded"}, {"lastDefaultName", lastDefaultName_}})},
            {"applied", parametersJson(parameters_)}, {"lastError", lastError_}});
    }
    void tick() {
        if (quitting_ || shutdown_) return;
        if (state_ == "conflict-official-fxsound" || state_ == "yielded" || paused_) return;
        if (GetTickCount64() - lastOfficialCheck_ > 1000) {
            lastOfficialCheck_ = GetTickCount64();
            if (officialFxSoundRunning()) {
                state_ = "conflict-official-fxsound"; lastError_ = error("OFFICIAL_FXSOUND_RUNNING", "Official FxSound is running");
                stopAudio(); restoreOwnedState(); persist(false); return;
            }
        }
        if (endpointChanged.exchange(false)) {
            endpoints_ = enumerateEndpoints();
            std::wstring currentVirtual;
            unsigned virtualCount = 0;
            for (const auto& endpoint : endpoints_) if (endpoint.virtualDevice) { currentVirtual = endpoint.id; ++virtualCount; }
            const bool captureChanged = currentVirtual != virtualId_;
            virtualId_ = virtualCount == 1 ? currentVirtual : std::wstring();
            if (virtualId_.empty()) {
                stopAudio(); state_ = "idle-no-device";
                lastError_ = error(virtualCount ? "DEFAULT_DEVICE_CONFLICT" : "VIRTUAL_DEVICE_MISSING", "Virtual capture endpoint is unavailable");
                restoreOwnedState(); persist(false);
                return;
            }
            std::deque<DefaultEvent> events;
            { std::lock_guard<std::mutex> lock(defaultEventsMutex); events.swap(defaultEvents); }
            for (const auto& event : events) {
                bool external = false;
                { std::lock_guard<std::mutex> lock(defaultEventsMutex);
                  external = defaultNotifications.accept(event.role, event.id, virtualId_, event.time); }
                if (!external) continue;
                const auto& id = event.id;
                auto e = findEndpoint(id);
                if (!e || e->virtualDevice) continue;
                lastDefaultName_ = e->name;
                if (conflict_.observe(GetTickCount64())) {
                    state_ = "yielded"; stopAudio(); restoreOwnedState(); storage_.log("Default-device conflict; yielded"); persist(false); return;
                }
                if (mode_ == "follow") remember(id);
            }
            auto before = outputId_; selectOutput();
            const auto selected = findEndpoint(outputId_);
            const bool formatChanged = selected && (!selected->formatSupported || selected->channels != outputChannels_ || selected->sampleRate != outputRate_);
            if (before != outputId_ || captureChanged || formatChanged || !audio_) { stopAudio(); startAudio(); ++reinitCount_; lastReinitReason_ = "device-change"; persist(false); }
        }
        if (audio_) {
            audio_->processTimer();
            serviceOutputRecovery();
            processing_ = dsp_ && dsp_->audioFrames() > 0 && audio_->isPlaybackDeviceAvailable();
            if (dsp_) dsp_->collect();
        }
    }
    bool quitting() const { return quitting_; }
    void pause(bool pause) {
        if (shutdown_ || quitting_) return;
        if (paused_ == pause) return;
        paused_ = pause;
        if (pause) { stopAudio(); restoreOwnedState(); if (state_ != "yielded" && state_ != "conflict-official-fxsound") state_ = "idle-no-device"; persist(false); }
        else if (state_ != "yielded" && state_ != "conflict-official-fxsound") { endpoints_ = enumerateEndpoints(); selectOutput(); stopAudio(); startAudio(); ++reinitCount_; lastReinitReason_ = "session-resume"; }
    }
    bool shutdown() noexcept {
        if (shutdown_) return clean_;
        shutdown_ = true;
        try {
            if (registered_) { enumerator_->UnregisterEndpointNotificationCallback(notifications_.Get()); registered_ = false; persistentNotifications = false; }
            stopAudio();
            clean_ = restoreOwnedState();
            persist(clean_); storage_.log(clean_ ? "Clean shutdown" : "Shutdown restore incomplete");
        } catch (...) { clean_ = false; }
        { std::lock_guard<std::mutex> lock(activeLogMutex); if (activeLog == &storage_) activeLog = nullptr; }
        return clean_;
    }
private:
    const Endpoint* findEndpoint(const std::wstring& id) const { for (const auto& e : endpoints_) if (e.id == id) return &e; return nullptr; }
    void remember(const std::wstring& id) {
        preferred_.erase(std::remove(preferred_.begin(), preferred_.end(), id), preferred_.end()); preferred_.insert(preferred_.begin(), id);
        if (preferred_.size() > 64) preferred_.resize(64);
    }
    void selectOutput() {
        std::vector<std::wstring> active;
        for (const auto& e : endpoints_) if (!e.virtualDevice) active.push_back(e.id);
        outputId_ = chooseOutput(active, preferred_, mode_ == "fixed" ? fixedId_ : std::wstring());
    }
    Json savedState(bool clean) const {
        Json::Array preferred;
        for (const auto& id : preferred_) preferred.emplace_back(utf8(id));
        return Json::object({{"schema", 1}, {"instance", instance_}, {"pid", GetCurrentProcessId()}, {"cleanExit", clean},
            {"guardianReady", guardianReady_}, {"recoveryPending", recoveryPending_}, {"noDefaultSwitch", options_.noDefaultSwitch},
            {"state", state_}, {"lastError", lastError_},
            {"restore", restore_}, {"outputVolumes", outputVolumes_}, {"outputId", utf8(outputId_)}, {"mode", mode_}, {"fixedId", utf8(fixedId_)}, {"preferred", preferred},
            {"bufferMs", bufferMs_}, {"applied", persistedParametersJson(parameters_)}});
    }
    void persist(bool clean) { storage_.writeState(savedState(clean)); }
    bool restoreOwnedState() noexcept {
        try {
            return completePendingRecovery(recoveryPending_, [&] { return restoreRecorded(savedState(false)); });
        } catch (...) { return false; }
    }
    void operationFailed() noexcept {
        stopAudio(); state_ = "idle-no-device";
        lastError_ = error("INTERNAL", "Audio initialization failed");
        restoreOwnedState();
        try { persist(false); } catch (...) { quitting_ = true; }
    }
    void startGuardian(StartupDiagnostics& diagnostics) {
        diagnostics.step(StartupStep::guardianEvent);
        PrivateSecurity security;
        auto name = L"Local\\Hikari1U.SoundEngine.GuardReady." + wide(instance_);
        Handle ready(CreateEventW(security.get(), TRUE, FALSE, name.c_str()));
        const auto eventError = GetLastError();
        if (!ready.get() || eventError == ERROR_ALREADY_EXISTS) throw StartupNativeError("Guard handshake creation failed", eventError);
        diagnostics.step(StartupStep::guardianLaunch);
        auto exe = executablePath();
        std::wstring command = quoteWindows(exe) + L" guard --pid " + std::to_wstring(GetCurrentProcessId()) + L" --creation " + wide(processCreation(GetCurrentProcess())) + L" --instance " + wide(instance_) + L" --data-dir " + quoteWindows(options_.dataDir.wstring());
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) throw StartupNativeError("Guard launch failed", GetLastError());
        Handle process(child.hProcess), thread(child.hThread);
        diagnostics.step(StartupStep::guardianWait);
        const auto waited = WaitForSingleObject(ready.get(), 3000);
        if (waited != WAIT_OBJECT_0) throw StartupNativeError("Guard handshake timed out", waited == WAIT_FAILED ? GetLastError() : waited);
    }
    void startAudio(StartupDiagnostics* diagnostics = nullptr) {
        auto step = [&](StartupStep value) { if (diagnostics) diagnostics->step(value); };
        step(StartupStep::audioInitialize);
        if (state_ == "yielded" || state_ == "conflict-official-fxsound") return;
        if (paused_) {
            stopAudio(); state_ = "idle-no-device";
            const bool restored = restoreOwnedState();
            if (!restored) lastError_ = error("INTERNAL", "Paused recovery could not be completed");
            persist(false);
            if (!restored) { step(StartupStep::recoveryRestore); throw std::runtime_error("Paused recovery failed"); }
            return;
        }
        if (officialFxSoundRunning()) {
            stopAudio(); state_ = "conflict-official-fxsound";
            lastError_ = error("OFFICIAL_FXSOUND_RUNNING", "Official FxSound is running");
            restoreOwnedState(); persist(false); return;
        }
        auto inactive = [&](const char* code, const char* message) {
            stopAudio(); state_ = "idle-no-device"; lastError_ = error(code, message);
            restoreOwnedState(); persist(false);
        };
        if (!recoveryPending_) { step(StartupStep::outputRefresh); endpoints_ = enumerateEndpoints(); selectOutput(); }
        unsigned virtualCount = 0;
        std::wstring capture;
        for (const auto& endpoint : endpoints_) if (endpoint.virtualDevice) { ++virtualCount; capture = endpoint.id; }
        virtualId_ = virtualCount == 1 ? capture : std::wstring();
        if (virtualId_.empty()) { inactive(virtualCount ? "DEFAULT_DEVICE_CONFLICT" : "VIRTUAL_DEVICE_MISSING", "Virtual capture endpoint is unavailable"); return; }
        if (outputId_.empty()) { inactive("NO_OUTPUT_DEVICE", "Output endpoint is unavailable"); return; }
        try {
        step(StartupStep::outputEndpoint);
        auto output = findEndpoint(outputId_);
        if (!output || output->virtualDevice) throw std::runtime_error("Output unavailable");
        step(StartupStep::outputVolume);
        if (!output->volumeKnown) throw std::runtime_error("Output volume could not be read");
        static const unsigned rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
        if (!output->formatSupported || (output->channels != 2 && output->channels != 4 && output->channels != 6 && output->channels != 8) || std::find(std::begin(rates), std::end(rates), output->sampleRate) == std::end(rates)) {
            inactive("DEVICE_FORMAT_UNSUPPORTED", "Output format is unsupported"); return;
        }
        if (!recoveryPending_) {
            step(StartupStep::recoverySnapshot);
            std::vector<RecoveryRecord> fallbackRoles;
            for (const auto& record : restore_) fallbackRoles.push_back({static_cast<unsigned>(record.at("role").asNumber()),
                wide(record.at("id").asString()), static_cast<float>(record.at("volume").asNumber()), record.at("muted").asBool()});
            const auto snapshot = freshRecoverySnapshot(endpoints_, outputId_, fallbackRoles);
            Json::Array roles, volumes;
            for (const auto& record : snapshot.roles) roles.push_back(Json::object({{"role", record.role}, {"id", utf8(record.id)}, {"volume", record.volume}, {"muted", record.muted}}));
            for (const auto& record : snapshot.outputs) volumes.push_back(Json::object({{"id", utf8(record.id)}, {"volume", record.volume}, {"muted", record.muted}}));
            restore_ = std::move(roles); outputVolumes_ = std::move(volumes);
        }
        bool captured = false;
        for (const auto& saved : outputVolumes_) if (saved.at("id").asString() == utf8(outputId_)) captured = true;
        if (!captured) outputVolumes_.push_back(Json::object({{"id", utf8(outputId_)}, {"volume", output->volume}, {"muted", output->muted}}));
        // Commit recovery evidence before any upstream device or volume mutation.
        recoveryPending_ = true;
        step(StartupStep::recoveryWrite);
        persist(false);
        { std::lock_guard<std::mutex> lock(preferredOutputMutex); preferredOutput = outputId_; }
        step(StartupStep::dspCreate);
        auto dsp = std::make_unique<DspAdapter>();
        step(StartupStep::dspApply);
        dsp->apply(parameters_);
        step(StartupStep::dspFormat);
        dsp->prepareFormat(output->sampleRate % 48000 == 0 ? 48000 : 44100, output->channels);
        allowUpstreamSwitch = !options_.noDefaultSwitch;
        // Upstream init already runs a reinit, so its playback reports must count for this session.
        playbackInitSeen_ = playbackInitializeSequence(); outputRecovery_.reset(); outputInitialized_ = false;
        step(StartupStep::audioInitialize);
        auto audio = std::make_unique<AudioPassthru>();
        if (audio->init() != 0) throw std::runtime_error("Upstream initialization failed");
        audio->setDspProcessingModule(dsp->upstream());
        step(StartupStep::outputSelection);
        bool selected = false;
        for (const auto& device : audio->getSoundDevices()) if (device.pwszID == outputId_ && device.isRealDevice) { selected = true; break; }
        if (!selected) throw std::runtime_error("Upstream output selection failed");
        step(StartupStep::bufferInitialize);
        if (audio->setBufferLength(bufferMs_) != 0) throw std::runtime_error("Buffer initialization failed");
        audio_ = std::move(audio); dsp_ = std::move(dsp);
        outputChannels_ = output->channels; outputRate_ = output->sampleRate;
        state_ = "starting"; lastError_ = Json(); processing_ = false;
        } catch (...) { operationFailed(); throw; }
    }
    void stopAudio() {
        allowUpstreamSwitch = false;
        audio_.reset(); dsp_.reset(); processing_ = false;
        outputRecovery_.reset(); releaseOutputError(); outputInitialized_ = false;
        { std::lock_guard<std::mutex> lock(preferredOutputMutex); preferredOutput.clear(); }
    }
    void serviceOutputRecovery() {
        const std::uint64_t now = GetTickCount64();
        PlaybackInitializeReport report;
        if (takePlaybackInitializeReport(playbackInitSeen_, report)) {
            playbackInitSeen_ = report.sequence;
            outputInitialized_ = SUCCEEDED(report.hr);
            if (outputRecovery_.observe(report.hr, now)) {
                releaseOutputError();
                if (outputRecovery_.active()) {
                    const auto output = findEndpoint(outputId_);
                    outputError_ = outputRecovery_.lastError(output ? output->name : std::string());
                    lastError_ = outputError_;
                }
                const auto kind = classifyPlaybackInitialize(report.hr);
                storage_.log(kind.failed ? "Output failure " + std::string(kind.code) + " " + formatHresult(report.hr)
                    : "Output failure cleared " + formatHresult(report.hr));
                persist(false);
            }
        }
        // Act only on a parked upstream; a lock is probed directly because every upstream reinit rewrites the output volume.
        auto action = outputRecovery_.poll(now, !audio_->isPlaybackDeviceAvailable());
        if (action == OutputRecovery::Action::probe) action = outputRecovery_.probeResult(probeSharedOutputInitialize(outputId_), now);
        if (action == OutputRecovery::Action::kick) audio_->setBufferLength(bufferMs_);
    }
    void releaseOutputError() {
        if (!outputError_.isNull() && lastError_.stringify() == outputError_.stringify()) lastError_ = Json();
        outputError_ = Json();
    }
    Json devices() const { Json::Array list; for (const auto& e : enumerateEndpoints()) list.push_back(endpointJson(e)); return Json::object({{"ok", true}, {"devices", list}}); }
    RunOptions options_; Storage storage_; Parameters parameters_;
    std::vector<Endpoint> endpoints_; std::vector<std::wstring> preferred_; Json::Array restore_, outputVolumes_;
    std::wstring virtualId_, outputId_, fixedId_; std::string mode_ = "follow", state_ = "starting", instance_, lastDefaultName_, lastReinitReason_ = "startup";
    int bufferMs_ = 40; unsigned reinitCount_ = 0, outputChannels_ = 0, outputRate_ = 0;
    ULONGLONG started_ = GetTickCount64(), lastOfficialCheck_ = 0;
    Json lastError_, outputError_; DefaultConflict conflict_;
    OutputRecovery outputRecovery_; std::uint64_t playbackInitSeen_ = 0; bool outputInitialized_ = false;
    std::unique_ptr<DspAdapter> dsp_; std::unique_ptr<AudioPassthru> audio_;
    ComPtr<IMMDeviceEnumerator> enumerator_; ComPtr<Notifications> notifications_;
    bool registered_ = false, paused_ = false, quitting_ = false, processing_ = false, shutdown_ = false, clean_ = false;
    bool guardianReady_ = false, recoveryPending_ = false;
};
struct Pending {
    Json request;
    std::promise<Json> result;
    std::atomic<bool> cancelled{false};
};
struct WindowContext { Engine* engine; std::atomic<bool>* stop; PauseReasons pauses; };
static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto context = reinterpret_cast<WindowContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { context = reinterpret_cast<WindowContext*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context)); }
    try { if (context) {
        auto pause = [&](PauseReasons::Reason reason, bool active) { context->engine->pause(context->pauses.set(reason, active)); };
        if (message == WM_QUERYENDSESSION) { pause(PauseReasons::ending, true); return TRUE; }
        if (message == WM_ENDSESSION) { if (w) { context->engine->shutdown(); *context->stop = true; } else pause(PauseReasons::ending, false); return 0; }
        if (message == WM_POWERBROADCAST) { if (w == PBT_APMSUSPEND) pause(PauseReasons::power, true); else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) pause(PauseReasons::power, false); return TRUE; }
        if (message == WM_WTSSESSION_CHANGE) {
            if (w == WTS_CONSOLE_DISCONNECT) pause(PauseReasons::console, true);
            if (w == WTS_REMOTE_CONNECT) pause(PauseReasons::remote, true);
            if (w == WTS_SESSION_LOCK) pause(PauseReasons::locked, true);
            if (w == WTS_CONSOLE_CONNECT) pause(PauseReasons::console, false);
            if (w == WTS_REMOTE_DISCONNECT) pause(PauseReasons::remote, false);
            if (w == WTS_SESSION_UNLOCK) pause(PauseReasons::locked, false);
        }
    } } catch (...) { if (context) *context->stop = true; return 0; }
    return DefWindowProcW(window, message, w, l);
}
int runEngine(const RunOptions& options) {
    auto endpoints = enumerateEndpoints();
    auto virtualCount = std::count_if(endpoints.begin(), endpoints.end(), [](const Endpoint& e) { return e.virtualDevice; });
    // No constructor, file, registry, process, event or device mutation is allowed before this check.
    if (virtualCount == 0) { outputLine(error("VIRTUAL_DEVICE_MISSING", "Virtual audio endpoint is unavailable")); return 2; }
    if (virtualCount != 1) { outputLine(error("DEFAULT_DEVICE_CONFLICT", "Multiple virtual audio endpoints are present")); return 2; }
    if (!options.captureId.empty()) {
        auto chosen = std::find_if(endpoints.begin(), endpoints.end(), [&](const Endpoint& e) { return e.id == options.captureId && e.virtualDevice; });
        if (chosen == endpoints.end()) { outputLine(error("DEVICE_FORMAT_UNSUPPORTED", "Capture endpoint must be the virtual audio endpoint")); return 2; }
    }
    if (officialFxSoundRunning()) { auto e = error("OFFICIAL_FXSOUND_RUNNING", "Official FxSound is running"); e["state"] = "conflict-official-fxsound"; outputLine(e); return 2; }
    Storage startupLog(options.dataDir);
    StartupDiagnostics diagnostics([&](const std::string& line) { startupLog.log(line); });
    StartupEnumerationScope enumerationScope(diagnostics);
    try {
    PrivateSecurity security;
    Handle owner(CreateMutexW(security.get(), FALSE, runMutexName));
    if (!owner.get()) throw StartupNativeError("Instance ownership failed", GetLastError());
    DWORD acquired = WaitForSingleObject(owner.get(), 5000);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) { outputLine(error("DEFAULT_DEVICE_CONFLICT", "An engine instance is already running")); return 2; }
    struct Release { HANDLE handle; ~Release() { ReleaseMutex(handle); } } release{owner.get()};
    diagnostics.step(StartupStep::outputRefresh);
    endpoints = enumerateEndpoints();
    virtualCount = std::count_if(endpoints.begin(), endpoints.end(), [](const Endpoint& e) { return e.virtualDevice; });
    if (virtualCount != 1) { outputLine(error(virtualCount ? "DEFAULT_DEVICE_CONFLICT" : "VIRTUAL_DEVICE_MISSING", "Virtual audio endpoint is unavailable")); return 2; }
    if (!options.captureId.empty() && std::none_of(endpoints.begin(), endpoints.end(), [&](const Endpoint& e) { return e.id == options.captureId && e.virtualDevice; })) {
        outputLine(error("DEVICE_FORMAT_UNSUPPORTED", "Capture endpoint must be the virtual audio endpoint")); return 2;
    }
    if (officialFxSoundRunning()) { auto e = error("OFFICIAL_FXSOUND_RUNNING", "Official FxSound is running"); e["state"] = "conflict-official-fxsound"; outputLine(e); return 2; }
    diagnostics.step(StartupStep::stopEvent);
    Handle event(CreateEventW(security.get(), TRUE, FALSE, stopEventName));
    if (!event.get()) throw StartupNativeError("Stop event failed", GetLastError()); ResetEvent(event.get());
    Engine engine(options, std::move(endpoints), diagnostics);
    std::mutex queueMutex; std::deque<std::shared_ptr<Pending>> queue;
    std::atomic<bool> stop{false};
    diagnostics.step(StartupStep::pipe);
    PipeServer pipe([&](const Json& request) {
        auto pending = std::make_shared<Pending>(); pending->request = request;
        auto result = pending->result.get_future();
        { std::lock_guard<std::mutex> lock(queueMutex); if (queue.size() >= 32 || stop) return Engine::withId(error("INTERNAL", "Engine is busy"), request); queue.push_back(pending); }
        if (result.wait_for(std::chrono::seconds(4)) != std::future_status::ready) { pending->cancelled = true; return Engine::withId(error("INTERNAL", "Engine request timed out"), request); }
        return result.get();
    });
    auto rejectQueued = [&] {
        std::deque<std::shared_ptr<Pending>> rejected;
        { std::lock_guard<std::mutex> lock(queueMutex); rejected.swap(queue); }
        for (const auto& pending : rejected) if (!pending->cancelled)
            pending->result.set_value(Engine::withId(error("INTERNAL", "Engine is stopping"), pending->request));
    };
    struct QueueCloser {
        std::function<void()> close;
        ~QueueCloser() { try { close(); } catch (...) {} }
    } queueCloser{[&] { stop = true; rejectQueued(); }};
    // Pipe-name reservation must succeed before taking over a default endpoint.
    pipe.start();
    engine.initialize(diagnostics);
    diagnostics.step(StartupStep::sessionClass);
    WNDCLASSW cls{}; cls.lpfnWndProc = windowProcedure; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"HikariSoundEngine.Session";
    if (!RegisterClassW(&cls)) throw StartupNativeError("Session window registration failed", GetLastError());
    WindowContext context{&engine, &stop};
    if (GetSystemMetrics(SM_REMOTESESSION)) context.pauses.set(PauseReasons::remote, true);
    diagnostics.step(StartupStep::sessionWindow);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"HikariSoundEngine", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, cls.hInstance, &context);
    if (!window) throw StartupNativeError("Session window creation failed", GetLastError());
    WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
    diagnostics.complete();
    auto previous = std::string();
    try {
        while (!stop && !engine.quitting() && WaitForSingleObject(event.get(), 0) != WAIT_OBJECT_0) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (!canDrainCommandQueue(stop.load(), engine.quitting(), WaitForSingleObject(event.get(), 0) == WAIT_OBJECT_0)) break;
            std::deque<std::shared_ptr<Pending>> requests;
            { std::lock_guard<std::mutex> lock(queueMutex); requests.swap(queue); }
            for (auto& request : requests) if (!request->cancelled) request->result.set_value(engine.dispatch(request->request));
            engine.tick();
            auto status = engine.status(); auto serialized = status.stringify();
            // Uptime is omitted from the event comparison to avoid pushing unchanged state every tick.
            auto comparable = status; comparable["stats"]["uptimeSec"] = 0;
            if (comparable.stringify() != previous) { previous = comparable.stringify(); status["event"] = "state"; pipe.broadcast(status); }
            HANDLE waitEvent = event.get();
            MsgWaitForMultipleObjects(1, &waitEvent, FALSE, 100, QS_ALLINPUT);
        }
    } catch (...) { stop = true; rejectQueued(); engine.shutdown(); pipe.stop(); WTSUnRegisterSessionNotification(window); DestroyWindow(window); throw; }
    stop = true;
    rejectQueued();
    engine.shutdown();
    if (engine.quitting()) Sleep(100);
    pipe.stop(); WTSUnRegisterSessionNotification(window); DestroyWindow(window);
    return 0;
    } catch (...) { diagnostics.report(); throw; }
}
static bool savedOwnerAlive(const Json& state) {
    const auto pidValue = state.at("pid").asNumber();
    if (!std::isfinite(pidValue) || pidValue < 1 || pidValue > MAXDWORD || std::floor(pidValue) != pidValue)
        throw JsonError("Invalid recovery process identity");
    const auto pid = static_cast<DWORD>(pidValue);
    const auto& instance = state.at("instance").asString();
    const auto prefix = std::to_string(pid) + "-";
    if (instance.compare(0, prefix.size(), prefix) != 0) throw JsonError("Invalid recovery instance identity");
    const auto creation = instance.substr(prefix.size());
    if (creation.empty() || creation.size() > 20 || creation.find_first_not_of("0123456789") != std::string::npos)
        throw JsonError("Invalid recovery creation identity");
    Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.get()) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return false;
        throw std::runtime_error("Recovery process identity unavailable");
    }
    if (processCreation(process.get()) != creation) return false;
    const auto waited = WaitForSingleObject(process.get(), 0);
    if (waited == WAIT_OBJECT_0) return false;
    if (waited != WAIT_TIMEOUT) throw std::runtime_error("Recovery process status unavailable");
    return true;
}
int runGuard(const RunOptions& options, unsigned long pid, const std::string& creation, const std::string& instance) {
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!parent.get() || processCreation(parent.get()) != creation) return 2;
    // Verify the process image before trusting a user-provided PID.
    std::vector<wchar_t> path(32768); DWORD count = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(parent.get(), 0, path.data(), &count) || _wcsicmp(std::wstring(path.data(), count).c_str(), executablePath().c_str()) != 0) return 2;
    auto readyName = L"Local\\Hikari1U.SoundEngine.GuardReady." + wide(instance);
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str()));
    if (!ready.get() || !SetEvent(ready.get())) return 2;
    if (WaitForSingleObject(parent.get(), INFINITE) != WAIT_OBJECT_0) return 2;
    PrivateSecurity security;
    Handle owner(CreateMutexW(security.get(), FALSE, runMutexName));
    if (!owner.get()) return 2;
    Storage storage(options.dataDir);
    for (;;) {
        const auto acquired = WaitForSingleObject(owner.get(), 250);
        const bool ownsMutex = acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED;
        if (!ownsMutex && acquired != WAIT_TIMEOUT) return 2;
        struct Release { HANDLE handle; bool owned; ~Release() { if (owned) ReleaseMutex(handle); } } release{owner.get(), ownsMutex};
        try {
            auto state = storage.read("state.json", false);
            if (!state.isObject()) throw JsonError("Invalid guard state");
            const bool newer = state.at("instance").asString() != instance;
            const bool successorReady = state.contains("guardianReady") && state.at("guardianReady").asBool();
            const bool clean = state.at("cleanExit").asBool();
            const bool pending = !state.contains("recoveryPending") || state.at("recoveryPending").asBool();
            const bool alive = ownsMutex && newer && savedOwnerAlive(state);
            const auto decision = guardDecision(ownsMutex, newer, successorReady, alive, clean, pending);
            if (decision == GuardDecision::wait) { if (ownsMutex) Sleep(250); continue; }
            if (decision == GuardDecision::retire || decision == GuardDecision::done) return 0;
            // Only the mutex owner may restore or mark recovery complete.
            validatedRecoveryArray(state.at("restore"), true);
            if (state.contains("outputVolumes")) validatedRecoveryArray(state.at("outputVolumes"), false);
            const bool restored = restoreRecorded(state);
            state["cleanExit"] = restored;
            if (restored) state["recoveryPending"] = false;
            storage.writeState(state); storage.log(restored ? "Guard restored after abnormal exit" : "Guard restore incomplete");
            return restored ? 0 : 2;
        } catch (...) {
            if (!ownsMutex) continue;
            storage.log("Guard could not validate restoration evidence"); return 2;
        }
    }
}
}
