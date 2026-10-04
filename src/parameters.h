// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace hikari {

constexpr std::size_t kMaxFilters = 128;
constexpr double kGraphicFrequencies[10] = {
    62.5, 121.5, 225, 416.5, 770.5, 1425, 2645, 4895, 9060, 13885
};

struct Filter {
    bool enabled = true;
    std::string type = "PK";
    double freq = 1000;
    double gain = 0;
    double q = 1;
};

struct GraphicBand {
    double freq = 1000;
    double gain = 0;
};

struct Effects {
    double clarity = 0;
    double ambience = 0;
    double surround = 0;
    double dynamicBoost = 0;
    double bass = 0;
    double treble = 0;
};

struct EqParameters {
    std::string mode = "off";
    double preamp = 0;
    std::vector<GraphicBand> points = {
        {62.5, 0}, {121.5, 0}, {225, 0}, {416.5, 0}, {770.5, 0},
        {1425, 0}, {2645, 0}, {4895, 0}, {9060, 0}, {13885, 0}
    };
    std::vector<Filter> filters;
};

struct Parameters {
    bool bypass = false;
    EqParameters eq;
    Effects effects;
};

bool isFilterType(const std::string& type) noexcept;
bool validateParameters(const Parameters& parameters, std::string* error = nullptr);
Parameters clampParameters(Parameters parameters);

} // namespace hikari
