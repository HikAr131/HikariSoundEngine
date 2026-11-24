// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "delta_profile.h"
#include <algorithm>
#include <cmath>

namespace hikari {
const char* boomName(BoomLevel level) noexcept {
    switch (level) {
    case BoomLevel::off: return "off";
    case BoomLevel::medium: return "medium";
    case BoomLevel::strong: return "strong";
    case BoomLevel::light: break;
    }
    return "light";
}

bool parseBoomLevel(const std::string& text, BoomLevel& level) noexcept {
    for (const auto candidate : {BoomLevel::off, BoomLevel::light, BoomLevel::medium, BoomLevel::strong}) {
        if (text != boomName(candidate)) continue;
        level = candidate;
        return true;
    }
    return false;
}

bool validateDelta(const DeltaProfile& profile) noexcept { return std::isfinite(profile.footstep); }

DeltaProfile clampDelta(DeltaProfile profile) noexcept {
    profile.footstep = std::isfinite(profile.footstep) ? std::clamp(std::round(profile.footstep), 0.0, 10.0) : 0;
    return profile;
}

bool sameDelta(const DeltaProfile& a, const DeltaProfile& b) noexcept {
    return a.enabled == b.enabled && a.wind == b.wind && a.footstep == b.footstep && a.boom == b.boom;
}

double footstepGainDb(double level) noexcept {
    const int step = static_cast<int>(std::clamp(std::isfinite(level) ? level : 0.0, 0.0, 10.0) * 12.7 + 0.5);
    const int tenths = static_cast<int>(step * 0.7);
    return tenths < 60 ? 0.1 * tenths : 6 + 0.2 * (tenths - 60);
}
} // namespace hikari
