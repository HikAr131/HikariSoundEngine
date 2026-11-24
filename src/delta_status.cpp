// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "delta_status.h"
#include <cmath>

namespace hikari {
DeltaSnapshot DeltaSnapshot::from(const DspAdapter::DeltaStatus& status) {
    DeltaSnapshot snapshot;
    snapshot.wind = status.wind;
    snapshot.footstep = status.footstepActive ? "active" : "off";
    snapshot.limiterWarning = status.limiterWarning;
    return snapshot;
}

DeltaSnapshot DeltaSnapshot::idle(const Parameters& parameters) {
    DeltaSnapshot snapshot;
    if (parameters.delta.wind) snapshot.wind.state = WindState::paused;
    return snapshot;
}

Json DeltaSnapshot::json() const {
    const auto rounded = [](double value, double step) { return std::isfinite(value) ? std::round(value / step) * step : 0.0; };
    return Json::object({
        {"wind", Json::object({{"state", windStateName(wind.state)}, {"reference", wind.reference < 0 ? Json() : Json(wind.reference + 1)},
            {"quality", rounded(wind.quality, 0.01)}, {"fc", rounded(wind.cutoff, 1)}, {"gainDb", rounded(wind.gainDb, 0.1)}})},
        {"footstep", Json::object({{"state", footstep}})},
        {"limiterWarning", limiterWarning}});
}

const DeltaSnapshot& DeltaEventGate::filter(const DeltaSnapshot& current, std::uint64_t now) {
    const bool changed = !known_ || current.wind.state != shown_.wind.state || current.wind.reference != shown_.wind.reference ||
        current.footstep != shown_.footstep || current.limiterWarning != shown_.limiterWarning ||
        std::abs(current.wind.quality - shown_.wind.quality) > kQualityStep;
    if (changed && (!known_ || now - shownAt_ >= kIntervalMs)) {
        shown_ = current;
        shownAt_ = now;
        known_ = true;
    }
    return shown_;
}
} // namespace hikari
