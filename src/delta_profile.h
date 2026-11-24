// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <string>

namespace hikari {
enum class BoomLevel { off, light, medium, strong };

struct DeltaProfile {
    bool enabled = false;
    bool wind = true;
    double footstep = 5;
    BoomLevel boom = BoomLevel::light;
};

const char* boomName(BoomLevel level) noexcept;
bool parseBoomLevel(const std::string& text, BoomLevel& level) noexcept;
bool validateDelta(const DeltaProfile& profile) noexcept;
DeltaProfile clampDelta(DeltaProfile profile) noexcept;
bool sameDelta(const DeltaProfile& a, const DeltaProfile& b) noexcept;
// Largest boost of the footstep stage for a 0-10 level; 0 turns the stage off.
double footstepGainDb(double level) noexcept;
} // namespace hikari
