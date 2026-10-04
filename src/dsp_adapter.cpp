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
namespace {
constexpr unsigned kMaxSyncAttempts = 16;
void raiseMaximum(std::atomic<std::uint64_t>& maximum, std::uint64_t value) noexcept {
    auto current = maximum.load();
    while (value > current && !maximum.compare_exchange_weak(current, value)) {}
}
}

// Holds the audio lock for one apply step and records how long the audio thread could
// have waited (wall time) and how much work happened inside (thread cycles).
class DspAdapter::TimedLock {
public:
    explicit TimedLock(DspAdapter& owner) : owner_(owner), lock_(owner.mutex_) {
        QueryThreadCycleTime(GetCurrentThread(), &cycles_);
        QueryPerformanceCounter(&start_);
    }
    ~TimedLock() {
        LARGE_INTEGER end{}, frequency{};
        ULONG64 cycles = 0;
        QueryPerformanceCounter(&end);
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        QueryPerformanceFrequency(&frequency);
        const auto ticks = static_cast<std::uint64_t>(end.QuadPart - start_.QuadPart);
        raiseMaximum(owner_.applyLockMaxNanoseconds_,
            static_cast<std::uint64_t>(static_cast<double>(ticks) * 1e9 / static_cast<double>(frequency.QuadPart)));
        raiseMaximum(owner_.applyLockMaxCycles_, cycles - cycles_);
    }
    TimedLock(const TimedLock&) = delete;
    TimedLock& operator=(const TimedLock&) = delete;
private:
    DspAdapter& owner_;
    std::lock_guard<std::mutex> lock_;
    LARGE_INTEGER start_{};
    ULONG64 cycles_ = 0;
};

DspAdapter::DspAdapter() : dsp_(std::make_unique<DfxDsp>()) {
    // Constant upstream state is set once; bypass is rendered by this adapter, so the
    // upstream power stays on and its effects keep running underneath a bypass fade.
    dsp_->powerOn(true);
    dsp_->eqOn(false);
    dsp_->setBalance(0);
    dsp_->setNormalization(0);
    dsp_->setVolumeLeveling(0);
    dsp_->setMasterGain(0);
    for (int effect = 0; effect < DfxDsp::NumEffects; ++effect)
        dsp_->setEffectValue(static_cast<DfxDsp::Effect>(effect), 0.0f);
    upstreamSetterCalls_ = 6 + DfxDsp::NumEffects;
    std::lock_guard<std::mutex> lock(adaptersMutex);
    adapters.emplace(dsp_.get(), this);
}
DspAdapter::~DspAdapter() {
    std::lock_guard<std::mutex> lock(adaptersMutex);
    adapters.erase(dsp_.get());
}
void DspAdapter::commitLocked(const Parameters& next) {
    const double values[] = {next.effects.clarity, next.effects.ambience,
        next.effects.surround, next.effects.dynamicBoost, next.effects.bass};
    for (int effect = 0; effect < DfxDsp::NumEffects; ++effect) {
        if (values[effect] == effects_[effect]) continue;
        dsp_->setEffectValue(static_cast<DfxDsp::Effect>(effect), static_cast<float>(values[effect]));
        effects_[effect] = values[effect];
        ++upstreamSetterCalls_;
    }
    bypassTarget_ = next.bypass;
}
void DspAdapter::apply(const Parameters& parameters) {
    std::string error;
    if (!validateParameters(parameters, &error)) throw std::invalid_argument(error);
    auto next = clampParameters(parameters);
    std::lock_guard<std::mutex> configurationLock(configurationMutex_);
    if (sameParameters(next, parameters_)) return;
    std::vector<std::unique_ptr<EqChain>> garbage;
    garbage.reserve(EqStage::kRetiredSlots);
    std::unique_ptr<EqChain> released;
    unsigned sampleRate = 0, channels = 0;
    {
        TimedLock lock(*this);
        eq_.takeRetired(garbage);
        if (!eq_.ready() || eq_.retarget(next, released)) {
            commitLocked(next);
            sampleRate = 0;
        } else {
            sampleRate = static_cast<unsigned>(sampleRate_);
            channels = static_cast<unsigned>(channels_);
        }
    }
    if (!sampleRate) { parameters_ = std::move(next); return; }
    // Design and warm-up are the expensive part and run without the audio lock.
    std::vector<float> scratch;
    auto chain = EqStage::build(next, sampleRate, channels);
    ++chainsBuilt_;
    bool current = eq_.warm(*chain, scratch);
    for (unsigned attempt = 1;; ++attempt) {
        if (!current) {
            chain = EqStage::build(next, sampleRate, channels);
            ++chainsBuilt_;
            current = eq_.warm(*chain, scratch);
        }
        {
            TimedLock lock(*this);
            if (!eq_.synced(*chain) && attempt >= kMaxSyncAttempts) {
                eq_.catchUpLocked(*chain, scratch);
                ++lockedCatchUps_;
            }
            if (eq_.synced(*chain)) {
                eq_.install(std::move(chain), released);
                commitLocked(next);
                break;
            }
        }
        current = eq_.catchUp(*chain, scratch);
    }
    parameters_ = std::move(next);
}
Parameters DspAdapter::applied() {
    std::lock_guard<std::mutex> lock(configurationMutex_);
    return parameters_;
}
void DspAdapter::prepareFormat(int sampleRate, int channels) {
    if ((channels != 2 && channels != 4 && channels != 6 && channels != 8) ||
        sampleRate < 22050 || sampleRate > 192000)
        throw std::invalid_argument("Unsupported DSP preparation format");
    std::lock_guard<std::mutex> configurationLock(configurationMutex_);
    auto chain = EqStage::build(parameters_, static_cast<unsigned>(sampleRate), static_cast<unsigned>(channels));
    ++chainsBuilt_;
    auto resources = EqStage::allocate(static_cast<unsigned>(sampleRate), static_cast<unsigned>(channels));
    std::vector<std::unique_ptr<EqChain>> garbage;
    garbage.reserve(EqStage::kRetiredSlots + 3);
    std::lock_guard<std::mutex> lock(mutex_);
    if (dsp_->setSignalFormat(32, channels, sampleRate, 32) != 0)
        throw std::runtime_error("DSP format preparation failed");
    eq_.reset(resources, std::move(chain), garbage);
    sampleRate_ = sampleRate;
    channels_ = channels;
    bypassPosition_ = bypassTarget_ ? eq_.fadeFrames() : 0;
}
int DspAdapter::process(float* buffer, int frames, int bits, int channels,
    int sampleRate, int validBits, int duplicates) {
    if (!buffer || frames < 0 || bits != 32 || validBits != 32 ||
        (channels != 2 && channels != 4 && channels != 6 && channels != 8) ||
        sampleRate < 22050 || sampleRate > 192000) return 1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (sampleRate != sampleRate_ || channels != channels_ || !eq_.ready()) return 1;
    int result = 0;
    for (int done = 0; done < frames;) {
        const int count = std::min(frames - done, static_cast<int>(InputHistory::kMaxWriteFrames));
        const int status = processLocked(buffer + static_cast<std::size_t>(done) * channels, count, duplicates);
        if (status != 0) result = status;
        done += count;
    }
    return result;
}
int DspAdapter::processLocked(float* buffer, int frames, int duplicates) {
    const std::uint64_t first = eq_.written();
    eq_.process(buffer, static_cast<std::size_t>(frames));
    constexpr double optimizerNominalGain = 0.966051;
    constexpr double sampleLimit = 16;
    for (std::size_t index = 0; index < static_cast<std::size_t>(frames) * channels_; ++index) {
        const double sample = std::isfinite(buffer[index]) ? buffer[index] : 0;
        buffer[index] = static_cast<float>(std::clamp(sample / optimizerNominalGain, -sampleLimit, sampleLimit));
    }
    const int result = dsp_->processAudio(reinterpret_cast<short*>(buffer),
        reinterpret_cast<short*>(buffer), frames, duplicates);
    if (mixBypass(buffer, frames, first)) {
        audioFrames_.fetch_add(static_cast<std::uint64_t>(frames));
        return 0;
    }
    if (result == 0) audioFrames_.fetch_add(static_cast<std::uint64_t>(frames));
    return result;
}
bool DspAdapter::mixBypass(float* buffer, int frames, std::uint64_t first) noexcept {
    const std::size_t total = eq_.fadeFrames();
    const auto channels = static_cast<std::size_t>(channels_);
    if (!bypassTarget_ && bypassPosition_ == 0) return false;
    if (bypassTarget_ && bypassPosition_ == total) {
        for (int frame = 0; frame < frames; ++frame)
            std::copy_n(eq_.dry(first + static_cast<std::uint64_t>(frame)), channels, buffer + frame * channels);
        return true;
    }
    for (int frame = 0; frame < frames; ++frame) {
        if (bypassTarget_) { if (bypassPosition_ < total) ++bypassPosition_; }
        else if (bypassPosition_ > 0) --bypassPosition_;
        if (bypassPosition_ == 0) continue;
        float* out = buffer + frame * channels;
        const float* dry = eq_.dry(first + static_cast<std::uint64_t>(frame));
        if (bypassPosition_ == total) { std::copy_n(dry, channels, out); continue; }
        const double gain = eq_.fadeGain(bypassPosition_);
        for (std::size_t channel = 0; channel < channels; ++channel)
            out[channel] = static_cast<float>(out[channel] + gain * (static_cast<double>(dry[channel]) - out[channel]));
    }
    return false;
}
void DspAdapter::collect() {
    std::vector<std::unique_ptr<EqChain>> garbage;
    garbage.reserve(EqStage::kRetiredSlots);
    std::lock_guard<std::mutex> lock(mutex_);
    eq_.takeRetired(garbage);
}
DspAdapter::Stats DspAdapter::stats() const noexcept {
    return {chainsBuilt_.load(), upstreamSetterCalls_.load(), lockedCatchUps_.load(),
        applyLockMaxNanoseconds_.load(), applyLockMaxCycles_.load()};
}
std::size_t DspAdapter::fadeFrames() {
    std::lock_guard<std::mutex> lock(mutex_);
    return eq_.fadeFrames();
}
double DspAdapter::fadeGain(std::size_t position) {
    std::lock_guard<std::mutex> lock(mutex_);
    return eq_.fadeGain(position);
}
std::vector<TransitionRecord> DspAdapter::transitions() {
    std::vector<TransitionRecord> records;
    records.reserve(EqStage::kTransitionRecords);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto count = eq_.transitionCount();
    const auto first = count > EqStage::kTransitionRecords ? count - EqStage::kTransitionRecords : 0;
    for (auto index = first; index < count; ++index) records.push_back(eq_.transition(index));
    return records;
}
std::uint64_t DspAdapter::completedTransitions() {
    std::lock_guard<std::mutex> lock(mutex_);
    return eq_.completedTransitions();
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
