// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "dsp_adapter.h"
#include "capture_clock.h"
#include "hikari_upstream_hooks.h"
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

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

namespace { thread_local hikari::CaptureClock captureClock; }
void hikariOnCapturePacket(int offset, int frames, unsigned long flags, unsigned long long position) noexcept {
    captureClock.packet(offset, frames, flags, position, GetTickCount64());
}

namespace hikari {
namespace {
// Only a configuration thread starved below real-time speed can miss this; the engine then
// reports the apply as failed instead of computing inside the audio lock.
constexpr ULONGLONG kSyncTimeoutMs = 5000;
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

DspAdapter::DspAdapter(std::filesystem::path windReferences)
    : dsp_(std::make_unique<DfxDsp>()), windReferences_(std::move(windReferences)) {
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
std::unique_ptr<WindStage> DspAdapter::windFor(const Parameters& next, int sampleRate, int channels) const {
    if (!next.delta.enabled || !next.delta.wind || !sampleRate) return nullptr;
    return std::make_unique<WindStage>(static_cast<unsigned>(sampleRate), static_cast<unsigned>(channels), windReferences_);
}
void DspAdapter::commitLocked(const Parameters& next, std::unique_ptr<WindStage>& wind) {
    const double values[] = {next.effects.clarity, next.effects.ambience,
        next.effects.surround, next.effects.dynamicBoost, next.effects.bass};
    for (int effect = 0; effect < DfxDsp::NumEffects; ++effect) {
        if (values[effect] == effects_[effect]) continue;
        dsp_->setEffectValue(static_cast<DfxDsp::Effect>(effect), static_cast<float>(values[effect]));
        effects_[effect] = values[effect];
        ++upstreamSetterCalls_;
    }
    bypassTarget_ = next.bypass;
    if (wind) std::swap(wind_, wind);
    const bool windWanted = next.delta.enabled && next.delta.wind;
    if (wind_) {
        wind_->setActive(windWanted);
        windIdleSince_ = windWanted ? 0 : GetTickCount64();
    }
    if (post_) post_->apply(next.delta);
    if (next.delta.enabled == deltaTarget_) return;
    deltaTarget_ = next.delta.enabled;
    const bool settled = deltaPosition_ == 0 || deltaPosition_ == deltaFade_;
    const bool already = deltaPosition_ == (deltaTarget_ ? deltaFade_ : 0);
    if (deltaTarget_ && deltaPosition_ == 0 && post_) post_->reset();
    prime_ = settled && !already ? primeFrames_ : 0;
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
    // Only the configuration side installs or removes the stage, so its presence can be read without the audio lock.
    auto wind = wind_ ? nullptr : windFor(next, sampleRate_, channels_);
    {
        TimedLock lock(*this);
        eq_.takeRetired(garbage);
        if (!eq_.ready() || eq_.retarget(next, released)) {
            commitLocked(next, wind);
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
    // Catching up never happens under the audio lock: the lock only confirms the chain is level
    // with the history. A chain that fell out of the readable history is rebuilt and warmed again.
    const auto deadline = GetTickCount64() + kSyncTimeoutMs;
    for (;;) {
        if (current) {
            TimedLock lock(*this);
            if (eq_.synced(*chain)) {
                eq_.install(std::move(chain), released);
                commitLocked(next, wind);
                break;
            }
        }
        if (GetTickCount64() > deadline) throw std::runtime_error("EQ chain could not catch up with the audio history");
        ++syncRetries_;
        if (current) {
            std::this_thread::yield();
            current = eq_.catchUp(*chain, scratch);
        } else {
            chain = EqStage::build(next, sampleRate, channels);
            ++chainsBuilt_;
            current = eq_.warm(*chain, scratch);
        }
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
    auto wind = windFor(parameters_, sampleRate, channels);
    auto post = std::make_unique<DeltaPost>();
    post->prepare(sampleRate, static_cast<unsigned>(channels));
    post->apply(parameters_.delta);
    post->reset();
    std::vector<float> original(InputHistory::kMaxWriteFrames * static_cast<std::size_t>(channels));
    std::vector<float> delta(original.size());
    std::lock_guard<std::mutex> lock(mutex_);
    if (dsp_->setSignalFormat(32, channels, sampleRate, 32) != 0)
        throw std::runtime_error("DSP format preparation failed");
    eq_.reset(resources, std::move(chain), garbage);
    sampleRate_ = sampleRate;
    channels_ = channels;
    bypassPosition_ = bypassTarget_ ? eq_.fadeFrames() : 0;
    std::swap(wind_, wind);
    windIdleSince_ = 0;
    std::swap(post_, post);
    std::swap(original_, original);
    std::swap(deltaBuffer_, delta);
    deltaFade_ = static_cast<std::size_t>(std::lround(kDeltaFadeSeconds * sampleRate));
    primeFrames_ = static_cast<std::size_t>(std::lround(kPrimeSeconds * sampleRate));
    deltaPosition_ = deltaTarget_ ? deltaFade_ : 0;
    prime_ = 0;
}
int DspAdapter::process(float* buffer, int frames, int bits, int channels,
    int sampleRate, int validBits, int duplicates) {
    if (!buffer || frames < 0 || bits != 32 || validBits != 32 ||
        (channels != 2 && channels != 4 && channels != 6 && channels != 8) ||
        sampleRate < 22050 || sampleRate > 192000) return 1;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto clock = captureClock;
    const int gaps = clock.count();
    captureClock.clear();
    if (sampleRate != sampleRate_ || channels != channels_ || !eq_.ready()) return 1;
    int result = 0, nextGap = 0;
    for (int done = 0; done < frames;) {
        int count = std::min(frames - done, static_cast<int>(InputHistory::kMaxWriteFrames));
        while (nextGap < gaps && clock.gap(nextGap).offset <= done) {
            if (wind_) wind_->gap(clock.gap(nextGap).frames);
            ++nextGap;
        }
        if (nextGap < gaps) count = std::min(count, clock.gap(nextGap).offset - done);
        const int status = processLocked(buffer + static_cast<std::size_t>(done) * channels, count, duplicates);
        if (status != 0) result = status;
        done += count;
    }
    return result;
}
int DspAdapter::upstreamLocked(float* buffer, int frames, int duplicates) {
    constexpr double optimizerNominalGain = 0.966051;
    constexpr double sampleLimit = 16;
    for (std::size_t index = 0; index < static_cast<std::size_t>(frames) * channels_; ++index) {
        const double sample = std::isfinite(buffer[index]) ? buffer[index] : 0;
        buffer[index] = static_cast<float>(std::clamp(sample / optimizerNominalGain, -sampleLimit, sampleLimit));
    }
    ++upstreamBlocks_;
    return dsp_->processAudio(reinterpret_cast<short*>(buffer), reinterpret_cast<short*>(buffer), frames, duplicates);
}
int DspAdapter::processLocked(float* buffer, int frames, int duplicates) {
    const std::uint64_t first = eq_.written();
    const std::size_t samples = static_cast<std::size_t>(frames) * channels_;
    const float* original = nullptr;
    if (wind_) {
        // The equalizer history only holds what is left after the wind; a bypass needs what came in.
        if (bypassTarget_ || bypassPosition_ > 0) { std::copy_n(buffer, samples, original_.data()); original = original_.data(); }
        wind_->process(buffer, static_cast<std::size_t>(frames));
    }
    eq_.process(buffer, static_cast<std::size_t>(frames));
    int result = 0;
    if (!deltaTarget_ && deltaPosition_ == 0) result = upstreamLocked(buffer, frames, duplicates);
    else if (deltaTarget_ && deltaPosition_ == deltaFade_) post_->process(buffer, static_cast<std::size_t>(frames));
    else {
        std::copy_n(buffer, samples, deltaBuffer_.data());
        post_->process(deltaBuffer_.data(), static_cast<std::size_t>(frames));
        result = upstreamLocked(buffer, frames, duplicates);
        for (int frame = 0; frame < frames; ++frame) {
            if (prime_ > 0) --prime_;
            else if (deltaTarget_) { if (deltaPosition_ < deltaFade_) ++deltaPosition_; }
            else if (deltaPosition_ > 0) --deltaPosition_;
            const double phase = static_cast<double>(deltaPosition_) / static_cast<double>(deltaFade_);
            const double mix = 0.5 - 0.5 * std::cos(3.141592653589793238462643383279502884 * phase);
            float* out = buffer + static_cast<std::size_t>(frame) * channels_;
            const float* delta = deltaBuffer_.data() + static_cast<std::size_t>(frame) * channels_;
            for (int channel = 0; channel < channels_; ++channel)
                out[channel] = static_cast<float>(out[channel] + mix * (static_cast<double>(delta[channel]) - out[channel]));
        }
    }
    if (mixBypass(buffer, frames, first, original)) {
        audioFrames_.fetch_add(static_cast<std::uint64_t>(frames));
        return 0;
    }
    if (result == 0) audioFrames_.fetch_add(static_cast<std::uint64_t>(frames));
    return result;
}
bool DspAdapter::mixBypass(float* buffer, int frames, std::uint64_t first, const float* original) noexcept {
    const std::size_t total = eq_.fadeFrames();
    const auto channels = static_cast<std::size_t>(channels_);
    const auto dryFrame = [&](int frame) {
        return original ? original + static_cast<std::size_t>(frame) * channels : eq_.dry(first + static_cast<std::uint64_t>(frame));
    };
    if (!bypassTarget_ && bypassPosition_ == 0) return false;
    if (bypassTarget_ && bypassPosition_ == total) {
        for (int frame = 0; frame < frames; ++frame) std::copy_n(dryFrame(frame), channels, buffer + frame * channels);
        return true;
    }
    for (int frame = 0; frame < frames; ++frame) {
        if (bypassTarget_) { if (bypassPosition_ < total) ++bypassPosition_; }
        else if (bypassPosition_ > 0) --bypassPosition_;
        if (bypassPosition_ == 0) continue;
        float* out = buffer + frame * channels;
        const float* dry = dryFrame(frame);
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
    // Stopping the tracking thread takes a moment, so a finished stage is destroyed after both locks are released.
    std::unique_ptr<WindStage> wind;
    std::lock_guard<std::mutex> configurationLock(configurationMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    eq_.takeRetired(garbage);
    if (wind_ && windIdleSince_ && wind_->silent() &&
        (!parameters_.delta.enabled || GetTickCount64() - windIdleSince_ >= kWindKeepMs)) std::swap(wind, wind_);
}
DspAdapter::Stats DspAdapter::stats() const noexcept {
    return {chainsBuilt_.load(), upstreamSetterCalls_.load(), syncRetries_.load(),
        applyLockMaxNanoseconds_.load(), applyLockMaxCycles_.load(), upstreamBlocks_.load()};
}
DspAdapter::DeltaStatus DspAdapter::deltaStatus() {
    std::lock_guard<std::mutex> lock(configurationMutex_);
    DeltaStatus status;
    const auto& delta = parameters_.delta;
    if (delta.wind) {
        if (!delta.enabled || parameters_.bypass) status.wind.state = WindState::paused;
        else if (wind_) status.wind = wind_->status();
        else status.wind.state = WindState::loading;
    }
    status.footstepActive = delta.enabled && delta.footstep > 0;
    status.limiterWarning = delta.enabled && post_ && post_->limiterWarning();
    return status;
}
DspAdapter::DeltaLatency DspAdapter::deltaLatency() {
    std::lock_guard<std::mutex> lock(configurationMutex_);
    if (!post_) return {};
    return {0, post_->footstepFrames(), post_->boomFrames(), post_->limiterFrames(),
        static_cast<std::size_t>(std::lround(0.00075 * sampleRate_))};
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
