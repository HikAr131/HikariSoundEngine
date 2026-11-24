// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include <windows.h>
#include <mmdeviceapi.h>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
// The fields under test are private to the pinned upstream classes; nothing else is reached this way.
#define private public
#include "u_AudioPassthru.h"
#undef private
#include "runtime.h"

namespace {
template <class Field>
unsigned char firstByte(const Field& field) {
    unsigned char value = 0;
    std::memcpy(&value, &field, sizeof(value));
    return value;
}
}

namespace hikari {
namespace {
// A worker that outlives the upstream timeout must keep its object, and no new audio may start before it ends.
void testStuckWorker() {
    if (audioWorkerStuck()) throw std::runtime_error("A stuck worker is reported before one exists");
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE worker = CreateThread(nullptr, 0, [](LPVOID event) -> DWORD { WaitForSingleObject(static_cast<HANDLE>(event), 60000); return 0; }, release, 0, nullptr);
    if (!release || !worker) throw std::runtime_error("Stuck worker fixture failed");
    auto audio = createAudioPassthru();
    auto* kept = audio->data_;
    kept->hProcessingThread_ = worker;
    audio.reset();
    const bool stuck = audioWorkerStuck();
    // Only an object that is still allocated can be asked again, and only then does the worker have to be gone.
    SetEvent(release);
    WaitForSingleObject(worker, 5000);
    const bool ended = !audioWorkerStuck();
    const bool stopped = kept->hikariWorkerStopped();
    CloseHandle(release);
    if (!stuck || !ended || !stopped) throw std::runtime_error("A worker that did not stop in time is not kept apart");
}
}
void runAudioPassthruTests() {
    testStuckWorker();
    constexpr unsigned char poison = 0x41;
    // The heap may hand out another block, so retry until an object lands on the poisoned one. The DSP pointer is
    // just as unwritten at this point, which makes it the witness that this object really reused that memory.
    for (int attempt = 0; attempt < 1024; ++attempt) {
        void* block = ::operator new(sizeof(AudioPassthruPrivate));
        std::memset(block, poison, sizeof(AudioPassthruPrivate));
        ::operator delete(block);
        auto audio = createAudioPassthru();
        const bool reused = firstByte(audio->data_->p_dfx_dsp_) == poison;
        const auto mute = firstByte(audio->data_->mute_);
        // Destroying the upstream object restores the Windows default device, so it stays allocated:
        // the self-test must not touch audio devices, and constructing the object does not.
        static_cast<void>(audio.release());
        if (!reused) continue;
        if (mute != 0) throw std::runtime_error("A new audio object can start muted");
        return;
    }
    throw std::runtime_error("Poisoned memory never reached the upstream audio object");
}
}
