// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
// Real-time properties of the profile that the self-test inside the engine cannot check: no allocation on the
// audio thread, the cost of one block, the cost of the tracking thread, and a lock with the thread running.
#include "dsp_adapter.h"
#include "security.h"
#include "hikari_upstream_logging.h"
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <vector>

bool hikariAllowDefaultSwitch(const wchar_t*) { return false; }
void hikariOnDefaultDeviceChanged(int, int, const wchar_t*) {}
bool hikariGetPreferredOutput(wchar_t*, int) { return false; }
void hikariOnPlaybackInitializeResult(HRESULT) noexcept {}
void hikariOnWorkerStuck(HANDLE) noexcept {}
void hikariBeginEnumeration() noexcept {}
void hikariOnEnumerationFailure(int, HRESULT) noexcept {}
bool hikariValidateAudioInitialization(const WAVEFORMATEX*, const WAVEFORMATEX*) noexcept { return true; }
extern "C" void hikariLogUpstreamError(const wchar_t*) {}

namespace {
thread_local bool counting = false;
thread_local unsigned long long allocations = 0;
void* counted(std::size_t size) {
    if (counting) ++allocations;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
double threadSeconds(HANDLE thread) {
    FILETIME created{}, ended{}, kernel{}, user{};
    GetThreadTimes(thread, &created, &ended, &kernel, &user);
    const auto value = [](const FILETIME& time) { return (static_cast<unsigned long long>(time.dwHighDateTime) << 32 | time.dwLowDateTime) / 1e7; };
    return value(kernel) + value(user);
}
double processSeconds() {
    FILETIME created{}, ended{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user);
    const auto value = [](const FILETIME& time) { return (static_cast<unsigned long long>(time.dwHighDateTime) << 32 | time.dwLowDateTime) / 1e7; };
    return value(kernel) + value(user);
}
}
void* operator new(std::size_t size) { return counted(size); }
void* operator new[](std::size_t size) { return counted(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    using namespace hikari;
    try {
        DspTestRegistry isolation;
        const auto file = std::filesystem::path(executablePath()).parent_path() / WindReferenceSet::kFileName;
        const auto references = WindReferenceSet::load(file);
        constexpr int rate = 48000, channels = 2, block = 480;
        constexpr double seconds = 12;
        DspAdapter adapter(file);
        Parameters parameters;
        parameters.eq.mode = "parametric";
        for (const double frequency : {31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0})
            parameters.eq.filters.push_back({true, "PK", frequency, 2, 1.4});
        parameters.delta = {true, true, 5, BoomLevel::light};
        adapter.apply(parameters);
        adapter.prepareFormat(rate, channels);
        std::vector<float> buffer(block * channels);
        std::vector<double> costs;
        costs.reserve(static_cast<std::size_t>(seconds * rate / block) + 1);
        LARGE_INTEGER frequency{}, begin{}, end{}, start{};
        QueryPerformanceFrequency(&frequency);
        unsigned noise = 1;
        bool locked = false;
        double lockedAt = -1, audioBefore = threadSeconds(GetCurrentThread()), processBefore = processSeconds(), processing = 0;
        QueryPerformanceCounter(&start);
        for (long long frame = 0; frame < static_cast<long long>(seconds * rate); frame += block) {
            for (int index = 0; index < block; ++index) for (int channel = 0; channel < channels; ++channel) {
                noise = noise * 1664525u + 1013904223u;
                double value = (static_cast<double>(noise) / 2147483648.0 - 1) * 0.001;
                if (references) {
                    const auto& item = references->items()[0];
                    value += item.samples[(static_cast<std::size_t>(400000 + frame + index) % item.frames) * 2 + static_cast<std::size_t>(channel)] / 65536.0;
                }
                if ((frame + index) % 96000 < 4800) value += (static_cast<double>(noise >> 8) / 8388608.0 - 1) * 0.5;
                buffer[static_cast<std::size_t>(index * channels + channel)] = static_cast<float>(value);
            }
            allocations = 0;
            QueryPerformanceCounter(&begin);
            counting = true;
            const int result = adapter.process(buffer.data(), block, 32, channels, rate, 32, 0);
            counting = false;
            QueryPerformanceCounter(&end);
            if (result != 0) throw std::runtime_error("processing failed");
            if (allocations) throw std::runtime_error("the audio thread allocates: " + std::to_string(allocations));
            costs.push_back(static_cast<double>(end.QuadPart - begin.QuadPart) / static_cast<double>(frequency.QuadPart));
            processing += costs.back();
            if (!locked && adapter.deltaStatus().wind.state == WindState::locked) { locked = true; lockedAt = static_cast<double>(frame) / rate; }
            // Feed at the speed of real audio.
            for (;;) {
                QueryPerformanceCounter(&end);
                const double ahead = static_cast<double>(frame + block) / rate - static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(frequency.QuadPart);
                if (ahead <= 0) break;
                Sleep(ahead > 0.002 ? 1 : 0);
            }
        }
        const double audio = threadSeconds(GetCurrentThread()) - audioBefore, background = processSeconds() - processBefore - audio;
        std::sort(costs.begin(), costs.end());
        const double worst = costs[static_cast<std::size_t>(static_cast<double>(costs.size() - 1) * 0.999)];
        const double length = static_cast<double>(block) / rate, share = background / seconds;
        std::printf("{\"ok\":true,\"blocks\":%zu,\"p999Us\":%.1f,\"blockShare\":%.4f,\"meanShare\":%.4f,\"trackingCpu\":%.4f,\"lockedAtSeconds\":%.2f,\"recordings\":%s,\"audioThreadAllocations\":0}\n",
            costs.size(), worst * 1e6, worst / length, processing / seconds, share, lockedAt, references ? "true" : "false");
        if (worst > length * 0.25) throw std::runtime_error("a block costs more than a quarter of its length");
        if (share > 0.06) throw std::runtime_error("the tracking thread uses more than 6% of a core");
        if (references && (!locked || lockedAt > 3.5)) throw std::runtime_error("the running tracking thread does not lock in time");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
