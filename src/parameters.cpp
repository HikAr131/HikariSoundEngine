// SPDX-License-Identifier: AGPL-3.0-or-later
#include "parameters.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hikari {

bool isFilterType(const std::string& type) noexcept {
    static const char* const types[] = {
        "PK", "LP", "HP", "LPQ", "HPQ", "BP", "LS", "HS",
        "LSC", "HSC", "LSQ", "HSQ", "NO", "AP"
    };
    for (const auto* candidate : types) {
        if (type == candidate) return true;
    }
    return false;
}

bool validateParameters(const Parameters& p, std::string* error) {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (p.eq.mode != "off" && p.eq.mode != "graphic" && p.eq.mode != "parametric")
        return fail("Invalid EQ mode");
    if (!std::isfinite(p.eq.preamp)) return fail("Non-finite preamp");
    if (p.eq.filters.size() > kMaxFilters) return fail("Too many filters");
    if (p.eq.points.empty() || p.eq.points.size() > 256) return fail("Expected one to 256 graphic points");
    double previousFrequency = 0;
    for (const auto& point : p.eq.points) {
        if (!std::isfinite(point.freq) || point.freq < 20 || point.freq > 20000
            || point.freq <= previousFrequency || !std::isfinite(point.gain))
            return fail("Invalid graphic point");
        previousFrequency = point.freq;
    }
    for (const auto& filter : p.eq.filters) {
        if (!isFilterType(filter.type)) return fail("Invalid filter type");
        if (!std::isfinite(filter.freq) || !std::isfinite(filter.gain)
            || !std::isfinite(filter.q)) return fail("Non-finite filter parameter");
    }
    const double levels[] = {p.effects.clarity, p.effects.ambience, p.effects.surround,
        p.effects.dynamicBoost, p.effects.bass, p.effects.treble};
    for (const auto level : levels) {
        if (!std::isfinite(level)) return fail("Non-finite effect level");
    }
    if (error) error->clear();
    return true;
}

Parameters clampParameters(Parameters p) {
    std::string error;
    if (!validateParameters(p, &error)) throw std::invalid_argument(error);
    p.eq.preamp = std::clamp(p.eq.preamp, -20.0, 20.0);
    for (auto& point : p.eq.points) point.gain = std::clamp(point.gain, -12.0, 12.0);
    for (auto& filter : p.eq.filters) {
        filter.freq = std::clamp(filter.freq, 20.0, 20000.0);
        filter.gain = std::clamp(filter.gain, -20.0, 20.0);
        filter.q = std::clamp(filter.q, 0.1, 10.0);
    }
    p.effects.clarity = std::clamp(p.effects.clarity, 0.0, 10.0);
    p.effects.ambience = std::clamp(p.effects.ambience, 0.0, 10.0);
    p.effects.surround = std::clamp(p.effects.surround, 0.0, 10.0);
    p.effects.dynamicBoost = std::clamp(p.effects.dynamicBoost, 0.0, 10.0);
    p.effects.bass = std::clamp(p.effects.bass, 0.0, 10.0);
    p.effects.treble = std::clamp(p.effects.treble, -100.0, 100.0);
    return p;
}

} // namespace hikari
