// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "dsp_adapter.h"
#include "hikari_upstream_hooks.h"
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>

namespace {
std::mutex adaptersMutex;
std::map<DfxDsp*, hikari::DspAdapter*> adapters;
std::mutex registryMutex;
std::wstring testRegistryRoot;
const std::wstring registryRoot = L"SOFTWARE\\Hikari1U\\SoundEngine";

bool ownRegistryPath(LPCWSTR path) {
    if (!path) return false;
    const std::wstring value(path);
    return value == registryRoot ||
        value.compare(0, registryRoot.size() + 1, registryRoot + L"\\") == 0;
}
std::wstring registryPath(LPCWSTR path) {
    std::lock_guard<std::mutex> lock(registryMutex);
    if (!testRegistryRoot.empty() && ownRegistryPath(path)) {
        return testRegistryRoot + std::wstring(path).substr(registryRoot.size());
    }
    return path ? path : L"";
}
} // namespace

LSTATUS WINAPI hikariRegCreateKeyExW(HKEY key, LPCWSTR path, DWORD reserved,
    LPWSTR className, DWORD options, REGSAM access,
    const LPSECURITY_ATTRIBUTES security, PHKEY result, LPDWORD disposition) {
    if (key != HKEY_CURRENT_USER || !ownRegistryPath(path)) return ERROR_ACCESS_DENIED;
    const auto actual = registryPath(path);
    if (actual != path) options = REG_OPTION_VOLATILE;
    return RegCreateKeyExW(key, actual.c_str(), reserved, className, options,
        access, security, result, disposition);
}
LSTATUS WINAPI hikariRegOpenKeyExW(HKEY key, LPCWSTR path, DWORD options,
    REGSAM access, PHKEY result) {
    const auto actual = registryPath(path);
    return RegOpenKeyExW(key, actual.c_str(), options, access, result);
}
LSTATUS WINAPI hikariRegDeleteKeyW(HKEY, LPCWSTR) { return ERROR_ACCESS_DENIED; }

extern "C" void hikariLogUpstreamErrorA(const char* message) {
    if (!message) return;
    const int length = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
    if (length <= 0) return;
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, message, -1, wide.data(), length) > 0)
        hikariLogUpstreamError(wide.c_str());
}

int hikariProcessAudio(DfxDsp* dsp, float* buffer, int frames, int bits,
    int channels, int sampleRate, int validBits, int duplicates) {
    std::lock_guard<std::mutex> lock(adaptersMutex);
    const auto found = adapters.find(dsp);
    if (found == adapters.end()) return 1;
    return found->second->process(buffer, frames, bits, channels, sampleRate, validBits, duplicates);
}

namespace hikari {
DspAdapter::DspAdapter() : dsp_(std::make_unique<DfxDsp>()) {
    apply(parameters_);
    std::lock_guard<std::mutex> lock(adaptersMutex);
    adapters.emplace(dsp_.get(), this);
}
DspAdapter::~DspAdapter() {
    std::lock_guard<std::mutex> lock(adaptersMutex);
    adapters.erase(dsp_.get());
}
void DspAdapter::apply(const Parameters& parameters) {
    std::string error;
    if (!validateParameters(parameters, &error)) throw std::invalid_argument(error);
    auto normalized = clampParameters(parameters);
    std::lock_guard<std::mutex> configurationLock(configurationMutex_);
    int sampleRate, channels;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sampleRate = sampleRate_;
        channels = channels_;
    }
    ParametricEq configured;
    if (sampleRate && channels) configured.configure(normalized, sampleRate, channels);
    std::lock_guard<std::mutex> lock(mutex_);
    std::swap(parameters_, normalized);
    dsp_->powerOn(!parameters_.bypass);
    dsp_->eqOn(false);
    dsp_->setBalance(0);
    dsp_->setNormalization(0);
    dsp_->setVolumeLeveling(0);
    dsp_->setMasterGain(0);
    const double values[] = {parameters_.effects.clarity, parameters_.effects.ambience,
        parameters_.effects.surround, parameters_.effects.dynamicBoost, parameters_.effects.bass};
    for (int effect = 0; effect < DfxDsp::NumEffects; ++effect) {
        dsp_->setEffectValue(static_cast<DfxDsp::Effect>(effect), static_cast<float>(values[effect]));
    }
    if (sampleRate && channels) std::swap(equalizer_, configured);
}
Parameters DspAdapter::applied() {
    std::lock_guard<std::mutex> lock(mutex_);
    return parameters_;
}
void DspAdapter::prepareFormat(int sampleRate, int channels) {
    if ((channels != 2 && channels != 4 && channels != 6 && channels != 8) ||
        sampleRate < 22050 || sampleRate > 192000)
        throw std::invalid_argument("Unsupported DSP preparation format");
    std::lock_guard<std::mutex> configurationLock(configurationMutex_);
    Parameters parameters;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        parameters = parameters_;
    }
    ParametricEq configured;
    configured.configure(parameters, sampleRate, channels);
    std::lock_guard<std::mutex> lock(mutex_);
    if (dsp_->setSignalFormat(32, channels, sampleRate, 32) != 0)
        throw std::runtime_error("DSP format preparation failed");
    std::swap(equalizer_, configured);
    sampleRate_ = sampleRate;
    channels_ = channels;
}
int DspAdapter::process(float* buffer, int frames, int bits, int channels,
    int sampleRate, int validBits, int duplicates) {
    if (!buffer || frames < 0 || bits != 32 || validBits != 32 ||
        (channels != 2 && channels != 4 && channels != 6 && channels != 8) ||
        sampleRate < 22050 || sampleRate > 192000) return 1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (sampleRate != sampleRate_ || channels != channels_) return 1;
    if (frames == 0) return 0;
    if (parameters_.bypass) {
        audioFrames_.fetch_add(static_cast<std::uint64_t>(frames));
        return 0;
    }
    equalizer_.process(buffer, static_cast<std::size_t>(frames));
    constexpr double optimizerNominalGain = 0.966051;
    constexpr double sampleLimit = 16;
    for (std::size_t index = 0; index < static_cast<std::size_t>(frames) * channels; ++index) {
        const double sample = std::isfinite(buffer[index]) ? buffer[index] : 0;
        buffer[index] = static_cast<float>(std::clamp(sample / optimizerNominalGain, -sampleLimit, sampleLimit));
    }
    const int result = dsp_->processAudio(reinterpret_cast<short*>(buffer),
        reinterpret_cast<short*>(buffer), frames, duplicates);
    if (result == 0) audioFrames_.fetch_add(static_cast<std::uint64_t>(frames));
    return result;
}
DspTestRegistry::DspTestRegistry() {
    GUID guid{}; wchar_t token[39]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, 39)) throw std::runtime_error("DSP test isolation token failed");
    path_ = L"SOFTWARE\\HikariSoundEngineSelfTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + token;
    std::lock_guard<std::mutex> lock(registryMutex);
    if (!testRegistryRoot.empty()) throw std::logic_error("nested DSP test registry");
    HKEY temporary = nullptr;
    DWORD disposition = 0;
    const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr,
        REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &temporary, &disposition);
    if (status != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
        if (temporary) RegCloseKey(temporary);
        throw std::runtime_error("DSP self-test registry isolation failed");
    }
    RegCloseKey(temporary);
    testRegistryRoot = path_;
}
DspTestRegistry::~DspTestRegistry() {
    std::lock_guard<std::mutex> lock(registryMutex);
    testRegistryRoot.clear();
    RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
}
} // namespace hikari
