// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "dsp_adapter.h"
#include "json.h"
#include <cstdint>
#include <string>

namespace hikari {
// What the pipe reports about the profile's own stages.
struct DeltaSnapshot {
    WindStatus wind;
    std::string footstep = "off";
    bool limiterWarning = false;
    static DeltaSnapshot from(const DspAdapter::DeltaStatus& status);
    static DeltaSnapshot idle(const Parameters& parameters);
    Json json() const;
};

// State events are only worth one push a second, and only when something a listener can see has changed.
class DeltaEventGate {
public:
    static constexpr std::uint64_t kIntervalMs = 1000;
    static constexpr double kQualityStep = 0.1;
    const DeltaSnapshot& filter(const DeltaSnapshot& current, std::uint64_t now);
private:
    DeltaSnapshot shown_;
    std::uint64_t shownAt_ = 0;
    bool known_ = false;
};
} // namespace hikari
