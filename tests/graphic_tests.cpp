// SPDX-License-Identifier: AGPL-3.0-or-later
#include "graphic_eq.h"
#include "json.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hikari {
namespace {
std::filesystem::path vectorFile() {
    const auto name = std::filesystem::path("graphic-vectors.json");
    std::vector<std::filesystem::path> candidates{std::filesystem::path("tests") / name, name};
#ifdef _WIN32
    std::vector<wchar_t> executable(32768);
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length && length < executable.size()) {
        auto directory = std::filesystem::path(std::wstring(executable.data(), length)).parent_path();
        candidates.push_back(directory / "test-vectors" / name);
        for (unsigned level = 0; level < 5; ++level) {
            candidates.push_back(directory / "tests" / name);
            directory = directory.parent_path();
        }
    }
#endif
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate;
    }
    throw std::runtime_error("Graphic self-test vectors are unavailable; include test-vectors beside the executable");
}

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void testReferenceCurves() {
    std::ifstream input(vectorFile(), std::ios::binary);
    require(input.good(), "Cannot open graphic self-test vectors");
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    const auto fixture = Json::parse(text);
    require(fixture.at("schema").asString() == "hikari-graphic-vectors-v1", "Invalid graphic vector schema");
    require(fixture.at("sources").asArray().size() == 2, "Missing graphic vector source hashes");
    for (const auto& source : fixture.at("sources").asArray()) {
        const auto& hash = source.at("sha256").asString();
        require(hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos,
            "Invalid graphic vector source hash");
    }
    const auto& presets = fixture.at("presets").asArray();
    require(presets.size() == 18, "Expected thirteen built-in and five synthetic graphic curves");
    unsigned builtins = 0;
    double worstBuiltin = 0;
    for (const auto& preset : presets) {
        std::vector<GraphicBand> points;
        for (const auto& point : preset.at("points").asArray())
            points.push_back({point.at("freq").asNumber(), point.at("gain").asNumber()});
        GraphicEq equalizer;
        equalizer.configure(points, 48000, 2);
        const auto& frequencies = preset.at("frequencies").asArray();
        const auto& expected = preset.at("expectedDb").asArray();
        require(frequencies.size() == 512 && expected.size() == frequencies.size(), "Incomplete graphic reference response");
        double maximum = 0, squareError = 0;
        for (std::size_t i = 0; i < frequencies.size(); ++i) {
            const double frequency = frequencies[i].asNumber();
            const double error = equalizer.responseDb(frequency) - expected[i].asNumber();
            require(std::isfinite(error), "Graphic EQ response is non-finite");
            maximum = std::max(maximum, std::abs(error));
            squareError += error * error;
        }
        const bool enforce = preset.at("enforceThreshold").asBool();
        if (enforce) {
            ++builtins;
            worstBuiltin = std::max(worstBuiltin, maximum);
            if (maximum > 1) throw std::runtime_error("Built-in graphic fidelity exceeds 1 dB: " + preset.at("key").asString());
        } else {
            std::cout << "Graphic synthetic " << preset.at("key").asString() << ": max=" << maximum
                      << " dB, RMS=" << std::sqrt(squareError / frequencies.size()) << " dB\n";
        }
        // This checks every emitted coefficient, not only the design-time transfer function.
        const auto& impulse = equalizer.impulseResponse();
        const auto delay = equalizer.latencyFrames();
        std::vector<float> audio((impulse.size() + delay + 512) * 2, 0);
        audio[0] = 1;
        equalizer.process(audio.data(), audio.size() / 2);
        for (std::size_t i = 0; i < audio.size() / 2; ++i) {
            const double reference = i >= delay && i - delay < impulse.size() ? impulse[i - delay] : 0;
            require(std::abs(audio[i * 2] - reference) < 1e-6, "Partition convolution did not reproduce its complete impulse response");
            require(audio[i * 2 + 1] == 0, "Graphic EQ leaked across channels");
        }
    }
    require(builtins == 13, "Incomplete built-in graphic fidelity coverage");
    std::cout << "Graphic built-in fidelity: worst max=" << worstBuiltin << " dB (13 curves, 512 frequencies each)\n";
}

void testStreaming() {
    const std::vector<GraphicBand> points{{20, -3}, {100, 2}, {2500, -4}, {20000, 3}};
    for (const unsigned channels : {2U, 4U, 6U, 8U}) {
        GraphicEq whole, split;
        whole.configure(points, 48000, channels); split.configure(points, 48000, channels);
        std::vector<float> signal(12000 * channels);
        unsigned random = 0x59a781f3;
        for (auto& value : signal) {
            random = random * 1664525u + 1013904223u;
            value = static_cast<float>((static_cast<double>(random) / 4294967296.0 - 0.5) * 0.05);
        }
        auto fragmented = signal;
        whole.process(signal.data(), 12000);
        for (std::size_t frame = 0; frame < 12000; frame += 137)
            split.process(fragmented.data() + frame * channels, std::min<std::size_t>(137, 12000 - frame));
        require(signal == fragmented, "Graphic convolution changed at input block boundaries");
        for (const auto value : signal) require(std::isfinite(value), "Graphic convolution produced a non-finite sample");
    }
    GraphicEq identity;
    identity.configure({{1000, 0}}, 48000, 2);
    float samples[] = {0.25f, -0.5f, 0.01f, 0};
    const std::vector<float> original(std::begin(samples), std::end(samples));
    identity.process(samples, 2);
    require(std::equal(original.begin(), original.end(), std::begin(samples)) && identity.latencyFrames() == 0,
        "Zero graphic EQ changed samples or added latency");
    require(GraphicEq::targetDb(points, 1) == -3 && GraphicEq::targetDb(points, 30000) == 3,
        "Graphic EQ did not hold constant gains outside the point range");
    std::vector<GraphicBand> dense;
    for (std::size_t i = 0; i < 256; ++i)
        dense.push_back({20 * std::pow(1000.0, static_cast<double>(i) / 255), 3 * std::sin(static_cast<double>(i) / 15)});
    dense.back().freq = 20000;
    GraphicEq arbitrary;
    arbitrary.configure(dense, 96000, 2);
    require(std::isfinite(arbitrary.responseDb(1000)), "256-point graphic curve was not supported");
}
}

void runGraphicTests() {
    testReferenceCurves();
    testStreaming();
}

} // namespace hikari
