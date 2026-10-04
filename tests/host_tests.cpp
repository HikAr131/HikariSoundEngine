// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "security.h"
#include "lifecycle.h"
#include <windows.h>
#include <stdexcept>
#include <functional>
#include <future>
#include <thread>
#include <atomic>

namespace hikari {
void runHostTests() {
    auto exe = executablePath();
    auto command = quoteWindows(exe) + L" --test-child";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) throw std::runtime_error("Guardian fixture launch failed");
    Handle process(info.hProcess), thread(info.hThread);
    auto created = processCreation(process.get());
    if (created.empty() || WaitForSingleObject(process.get(), 3000) != WAIT_OBJECT_0) throw std::runtime_error("Guardian fixture timeout");
    DWORD exit = 0;
    if (!GetExitCodeProcess(process.get(), &exit) || exit != 7) throw std::runtime_error("Guardian fixture exit unexpected");
    int restores = 0;
    std::function<void()> fakeRestore = [&] { ++restores; };
    for (bool clean : {false, true}) for (bool newer : {false, true}) {
        const int before = restores;
        if (shouldGuardRestore(clean, newer, false)) fakeRestore();
        if (restores != before + (!clean && !newer ? 1 : 0)) throw std::runtime_error("Guardian restoration decision failed");
    }
    if (restores != 1) throw std::runtime_error("Guardian restored more than once");
    PrivateSecurity security;
    const auto name = L"Local\\Hikari1U.SoundEngine.GuardFixture." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    Handle owner(CreateMutexW(security.get(), TRUE, name.c_str()));
    Handle busy(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!owner.get() || !busy.get()) throw std::runtime_error("Guardian handoff fixture failed");
    std::atomic<bool> alive{true};
    std::promise<void> completed;
    auto outcome = completed.get_future();
    std::thread guard([&] {
        bool owned = false;
        try {
            if (WaitForSingleObject(process.get(), 3000) != WAIT_OBJECT_0) throw std::runtime_error("Fake parent did not exit");
            if (WaitForSingleObject(owner.get(), 0) != WAIT_TIMEOUT ||
                guardDecision(false, true, false, alive, false, true) != GuardDecision::wait)
                throw std::runtime_error("Busy unready owner prematurely retired guardian");
            SetEvent(busy.get());
            const auto acquired = WaitForSingleObject(owner.get(), 2000);
            owned = acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED;
            if (!owned) throw std::runtime_error("Guardian did not acquire released owner");
            int recoveries = 0;
            bool clean = false, pending = true;
            if (guardDecision(true, true, false, alive, clean, pending) == GuardDecision::restore) {
                ++recoveries; clean = true; pending = false;
            }
            if (recoveries != 1 || guardDecision(true, false, false, false, clean, pending) != GuardDecision::done)
                throw std::runtime_error("Inherited recovery did not complete exactly once");
            ReleaseMutex(owner.get()); owned = false;
            completed.set_value();
        } catch (...) {
            if (owned) ReleaseMutex(owner.get());
            completed.set_exception(std::current_exception());
        }
    });
    const auto reachedBusy = WaitForSingleObject(busy.get(), 1000) == WAIT_OBJECT_0;
    alive = false;
    ReleaseMutex(owner.get());
    guard.join();
    outcome.get();
    if (!reachedBusy) throw std::runtime_error("Guardian never reached busy-owner branch");
    if (quoteWindows(L"a b\\\"c\\") != L"\"a b\\\\\\\"c\\\\\"") throw std::runtime_error("Windows argument quoting failed");
}
}
