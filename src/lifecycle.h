// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <deque>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hikari {
class PauseReasons {
public:
    enum Reason : unsigned { power = 1, console = 2, remote = 4, locked = 8, ending = 16 };
    bool set(Reason reason, bool active) { if (active) reasons_ |= reason; else reasons_ &= ~reason; return paused(); }
    bool paused() const { return reasons_ != 0; }
private:
    unsigned reasons_ = 0;
};
class DefaultConflict {
public:
    bool observe(std::uint64_t nowMs) {
        while (!events_.empty() && nowMs - events_.front() > 10000) events_.pop_front();
        events_.push_back(nowMs);
        return events_.size() >= 3;
    }
private:
    std::deque<std::uint64_t> events_;
};
inline bool acceptDefaultChange(const std::wstring& id, const std::wstring& virtualId, std::wstring& last) {
    if (id == virtualId) { last.clear(); return false; }
    if (id == last) return false;
    last = id; return true;
}
class DefaultNotifications {
public:
    void ownWrite(unsigned role, const std::wstring& id, std::uint64_t nowMs) {
        if (own_.size() >= 64) own_.pop_front();
        own_.push_back({role, id, nowMs});
    }
    bool accept(unsigned role, const std::wstring& id, const std::wstring& virtualId, std::uint64_t eventMs) {
        own_.erase(std::remove_if(own_.begin(), own_.end(), [&](const Own& item) {
            return eventMs >= item.time && eventMs - item.time > 5000;
        }), own_.end());
        const auto own = std::find_if(own_.begin(), own_.end(), [&](const Own& item) {
            const auto distance = eventMs >= item.time ? eventMs - item.time : item.time - eventMs;
            return item.role == role && item.id == id && distance <= 5000;
        });
        const bool ours = own != own_.end();
        if (ours) own_.erase(own);
        if (id == virtualId) { last_.clear(); return false; }
        if (ours) return false;
        return acceptDefaultChange(id, virtualId, last_);
    }
private:
    struct Own { unsigned role; std::wstring id; std::uint64_t time; };
    std::deque<Own> own_;
    std::wstring last_;
};
enum class GuardDecision { wait, retire, done, restore };
inline GuardDecision guardDecision(bool ownsMutex, bool newerInstance, bool guardianReady,
                                   bool newerOwnerAlive, bool clean, bool recoveryPending) {
    if (newerInstance && guardianReady) return GuardDecision::retire;
    if (!ownsMutex || (newerInstance && newerOwnerAlive)) return GuardDecision::wait;
    if (clean || !recoveryPending) return GuardDecision::done;
    return GuardDecision::restore;
}
inline bool canDispatchMutation(bool shutdown, bool quitting) { return !shutdown && !quitting; }
inline bool canDrainCommandQueue(bool stop, bool quitting, bool stopEvent) { return !stop && !quitting && !stopEvent; }
inline bool requiresModeRecovery(bool previousPending, bool previousNoDefaultSwitch, bool nextNoDefaultSwitch) {
    return previousPending && previousNoDefaultSwitch != nextNoDefaultSwitch;
}
template <class Restore>
bool completePendingRecovery(bool& pending, Restore restore) {
    if (!pending) return true;
    if (!restore()) return false;
    pending = false;
    return true;
}
struct RecoveryRecord { unsigned role; std::wstring id; float volume; bool muted; };
struct RecoverySnapshot { std::vector<RecoveryRecord> roles; std::vector<RecoveryRecord> outputs; };
template <class EndpointType>
RecoverySnapshot freshRecoverySnapshot(const std::vector<EndpointType>& endpoints, const std::wstring& outputId,
                                       const std::vector<RecoveryRecord>& fallbackRoles) {
    RecoverySnapshot snapshot;
    auto endpoint = [&](const std::wstring& id) -> const EndpointType* {
        for (const auto& e : endpoints) if (e.id == id && !e.virtualDevice) return &e;
        return nullptr;
    };
    auto record = [&](unsigned role, const EndpointType& e) {
        if (!e.volumeKnown || !std::isfinite(e.volume) || e.volume < 0 || e.volume > 1)
            throw std::runtime_error("Recovery snapshot volume is unavailable");
        return RecoveryRecord{role, e.id, e.volume, e.muted};
    };
    for (unsigned role : {0U, 1U}) {
        const EndpointType* selected = nullptr;
        for (const auto& e : endpoints)
            if (!e.virtualDevice && (role == 0 ? e.consoleDefault : e.multimediaDefault)) { selected = &e; break; }
        if (!selected) for (const auto& fallback : fallbackRoles)
            if (fallback.role == role) { selected = endpoint(fallback.id); if (selected) break; }
        if (!selected && role == 0) selected = endpoint(outputId);
        if (selected) snapshot.roles.push_back(record(role, *selected));
    }
    if (const auto* output = endpoint(outputId)) snapshot.outputs.push_back(record(0, *output));
    return snapshot;
}
inline bool shouldGuardRestore(bool cleanForInstance, bool newerInstance, bool /*noDefaultSwitch*/) {
    return !cleanForInstance && !newerInstance;
}
inline bool launchHintFresh(std::uint64_t writtenMs, std::uint64_t nowMs) {
    return writtenMs <= nowMs && nowMs - writtenMs <= 60000;
}
inline std::wstring chooseOutput(const std::vector<std::wstring>& active, const std::vector<std::wstring>& preferred, const std::wstring& fixed) {
    auto present = [&](const std::wstring& id) { return std::find(active.begin(), active.end(), id) != active.end(); };
    if (!fixed.empty()) return present(fixed) ? fixed : std::wstring();
    for (const auto& id : preferred) if (present(id)) return id;
    return active.empty() ? std::wstring() : active.front();
}
}
