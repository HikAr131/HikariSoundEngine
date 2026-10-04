// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "parameter_json.h"
#include <cmath>

namespace hikari {
static double number(const Json& object, const char* key, double fallback) {
    const Json* p = object.get(key);
    if (!p) return fallback;
    if (!p->isNumber() || !std::isfinite(p->asNumber())) throw JsonError("Expected finite numeric parameter");
    return p->asNumber();
}
static bool boolean(const Json& object, const char* key, bool fallback) {
    const Json* p = object.get(key);
    if (!p) return fallback;
    if (!p->isBool()) throw JsonError("Expected boolean parameter");
    return p->asBool();
}
Parameters parseParameters(const Json& input) {
    if (!input.isObject()) throw JsonError("Expected parameter object");
    Parameters p;
    p.bypass = boolean(input, "bypass", false);
    if (auto eq = input.get("eq")) {
        if (!eq->isObject()) throw JsonError("Expected EQ object");
        if (auto mode = eq->get("mode")) p.eq.mode = mode->asString();
        p.eq.preamp = number(*eq, "preamp", 0);
        if (eq->contains("bands")) throw JsonError("Graphic EQ requires points, not bands");
        if (auto points = eq->get("points")) {
            if (!points->isArray() || points->asArray().empty() || points->asArray().size() > 256) throw JsonError("Expected one to 256 EQ points");
            p.eq.points.clear();
            for (const auto& point : points->asArray()) {
                if (!point.isObject()) throw JsonError("Expected EQ point object");
                p.eq.points.push_back({number(point, "freq", 0), number(point, "gain", 0)});
            }
        }
        if (auto filters = eq->get("filters")) {
            if (!filters->isArray() || filters->asArray().size() > 128) throw JsonError("Too many EQ filters");
            for (const auto& item : filters->asArray()) {
                if (!item.isObject()) throw JsonError("Expected EQ filter object");
                Filter f;
                if (auto type = item.get("type")) f.type = type->asString();
                f.enabled = boolean(item, "enabled", true);
                f.freq = number(item, "freq", 1000); f.gain = number(item, "gain", 0); f.q = number(item, "q", 1);
                p.eq.filters.push_back(f);
            }
        }
    }
    if (auto e = input.get("effects")) {
        if (!e->isObject()) throw JsonError("Expected effects object");
        p.effects.clarity = number(*e, "clarity", 0); p.effects.ambience = number(*e, "ambience", 0);
        p.effects.surround = number(*e, "surround", 0); p.effects.dynamicBoost = number(*e, "dynamicBoost", 0);
        p.effects.bass = number(*e, "bass", 0); p.effects.treble = number(*e, "treble", 0);
    }
    std::string error;
    if (!validateParameters(p, &error)) throw JsonError(error);
    return clampParameters(std::move(p));
}
Json persistedParametersJson(const Parameters& parameters) {
    auto persisted = parameters;
    persisted.bypass = false;
    return parametersJson(persisted);
}
Parameters restoredParameters(const Json& input) {
    auto parameters = parseParameters(input);
    parameters.bypass = false;
    return parameters;
}
Json parametersJson(const Parameters& p) {
    Json::Array points, filters;
    for (const auto& b : p.eq.points) points.push_back(Json::object({{"freq", b.freq}, {"gain", b.gain}}));
    for (const auto& f : p.eq.filters) filters.push_back(Json::object({{"enabled", f.enabled}, {"type", f.type}, {"freq", f.freq}, {"gain", f.gain}, {"q", f.q}}));
    return Json::object({{"bypass", p.bypass}, {"eq", Json::object({{"mode", p.eq.mode}, {"preamp", p.eq.preamp}, {"points", points}, {"filters", filters}})},
        {"effects", Json::object({{"clarity", p.effects.clarity}, {"ambience", p.effects.ambience}, {"surround", p.effects.surround}, {"dynamicBoost", p.effects.dynamicBoost}, {"bass", p.effects.bass}, {"treble", p.effects.treble}})}});
}
}
