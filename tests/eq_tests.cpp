// SPDX-License-Identifier: AGPL-3.0-or-later
#include "parametric_eq.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>
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
    const auto name = std::filesystem::path("eq-vectors.json");
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
    throw std::runtime_error("EQ self-test vectors are unavailable; include test-vectors beside the executable");
}

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void readVectors() {
    std::ifstream input(vectorFile());
    require(input.good(), "Cannot open EQ self-test vectors");
    std::string line;
    bool vectors = false;
    std::size_t count = 0;
    std::size_t hashes = 0;
    std::set<std::string> labels;
    double maximumError = 0;
    while (std::getline(input, line)) {
        if (!vectors) {
            if (line.find("\"sha256\":") != std::string::npos) {
                const auto start = line.find('"', line.find(':') + 1);
                const auto end = line.find('"', start + 1);
                require(start != std::string::npos && end != std::string::npos && end - start == 65,
                    "Missing vector source hash");
                require(line.substr(start + 1, 64).find_first_not_of("0123456789abcdef") == std::string::npos,
                    "Invalid vector source hash");
                ++hashes;
            }
            if (line.find("\"vectors\": [") != std::string::npos) vectors = true;
            continue;
        }
        if (line.find(']') == line.find_first_not_of(" \t\r")) break;
        std::istringstream row(line);
        const auto punctuation = [&row](char expected) {
            char c = 0; row >> c;
            require(row.good() && c == expected, "Malformed EQ test vector");
        };
        punctuation('['); punctuation('"');
        Filter filter;
        std::getline(row, filter.type, '"');
        punctuation(','); row >> filter.freq;
        punctuation(','); row >> filter.gain;
        punctuation(','); row >> filter.q;
        double frequency = 0, expected = 0;
        punctuation(','); row >> frequency;
        punctuation(','); row >> expected;
        punctuation(']');
        require(std::isfinite(expected), "Non-finite reference EQ response");
        const auto actual = ParametricEq::responseDb(filter, frequency, 48000);
        require(std::isfinite(actual), "Non-finite calculated EQ response");
        const auto error = std::abs(actual - expected);
        maximumError = std::max(maximumError, error);
        if (error > 0.1) {
            std::ostringstream message;
            message << "EQ response mismatch: " << filter.type << " at " << frequency
                    << " Hz: expected " << expected << ", actual " << actual;
            throw std::runtime_error(message.str());
        }
        labels.insert(filter.type);
        ++count;
    }
    require(vectors && hashes == 2 && labels.size() == 14 && count == 14 * 4 * 256,
        "EQ test vectors are incomplete");
}

void testParameters() {
    Parameters p;
    p.eq.mode = "parametric";
    p.eq.preamp = 999;
    p.eq.filters.push_back({true, "PK", -30, 40, 0});
    p.eq.points.front().gain = -99;
    p.effects = {20, -1, 99, 50, -80, -999};
    p = clampParameters(p);
    require(p.eq.preamp == 20 && p.eq.filters[0].freq == 20 && p.eq.filters[0].gain == 20
        && p.eq.filters[0].q == 0.1 && p.eq.points.front().gain == -12, "EQ clamp failed");
    require(p.effects.clarity == 10 && p.effects.ambience == 0 && p.effects.surround == 10
        && p.effects.dynamicBoost == 10 && p.effects.bass == 0 && p.effects.treble == -100,
        "Effects clamp failed");
    p.eq.filters.resize(129);
    require(!validateParameters(p), "Oversized filter list was accepted");
    p.eq.filters.clear();
    p.eq.preamp = std::numeric_limits<double>::quiet_NaN();
    require(!validateParameters(p), "Non-finite preamp was accepted");
    p.eq.preamp = 0;
    p.eq.filters.push_back({true, "UNKNOWN", 1000, 0, 1});
    require(!validateParameters(p), "Unknown filter type was accepted");
    p.eq.filters.clear();
    p.eq.points = {{20, 0}, {80, 12}, {20000, -12}};
    require(validateParameters(p), "Arbitrary ascending graphic points were rejected");
    p.eq.points[1].freq = 20;
    require(!validateParameters(p), "Duplicate graphic frequencies were accepted");
    p.eq.points = {{19, 0}};
    require(!validateParameters(p), "Out-of-range graphic frequency was accepted");
    p.eq.points.assign(257, {1000, 0});
    require(!validateParameters(p), "Oversized graphic point list was accepted");
    p.eq.points.clear();
    require(!validateParameters(p), "Empty graphic point list was accepted");
}

void testProcessing() {
    const double rates[] = {8000, 22050, 44100, 48000, 96000, 192000};
    for (const auto rate : rates) {
        for (unsigned channels = 2; channels <= 8; ++channels) {
            Parameters p;
            p.eq.mode = "parametric";
            p.eq.preamp = -3;
            p.eq.filters = {{true, "PK", 1000, 6, 1.4}, {false, "LP", 100, 0, 1}};
            p.effects.treble = 50;
            ParametricEq eq;
            eq.configure(p, rate, channels);
            std::vector<float> samples(4096 * channels, 0);
            samples[0] = 1;
            eq.process(samples.data(), 4096);
            require(samples[0] != 1, "Active EQ did not process the impulse");
            for (std::size_t i = 0; i < samples.size(); ++i) {
                require(std::isfinite(samples[i]), "EQ produced a non-finite sample");
                if (i % channels != 0) require(samples[i] == 0, "EQ leaked across channels");
            }
            p.bypass = true;
            eq.configure(p, rate, channels);
            const auto original = samples;
            eq.process(samples.data(), 4096);
            require(std::memcmp(samples.data(), original.data(), samples.size() * sizeof(float)) == 0,
                "Bypass changed sample bytes");
            p.bypass = false;
            p.eq.mode = "off"; p.effects.treble = 0;
            eq.configure(p, rate, channels);
            eq.process(samples.data(), 4096);
            require(samples == original, "Off mode applied EQ or preamp");
        }
    }
    Parameters p;
    p.eq.mode = "parametric";
    p.eq.filters.push_back({true, "PK", 1000, 6, 1});
    ParametricEq eq;
    eq.configure(p, 48000, 2);
    std::vector<float> signal(96000 * 2);
    for (std::size_t i = 0; i < 96000; ++i)
        signal[i * 2] = signal[i * 2 + 1] = static_cast<float>(0.1 * std::sin(2 * 3.14159265358979323846 * i / 48));
    eq.process(signal.data(), 96000);
    double energy = 0;
    for (std::size_t i = 48000; i < 96000; ++i) energy += signal[i * 2] * signal[i * 2];
    const auto measured = 20 * std::log10(std::sqrt(energy / 48000) / (0.1 / std::sqrt(2.0)));
    require(std::abs(measured - 6) <= 0.01, "Process path did not match the PK response");
    p.eq.mode = "graphic";
    p.eq.preamp = 0; p.effects.treble = 0; p.eq.points = {{1000, 6}};
    eq.configure(p, 48000, 2);
    const auto copy = signal;
    eq.process(signal.data(), 96000);
    const double gain = std::pow(10.0, 6.0 / 20);
    for (std::size_t i = 0; i < signal.size(); ++i)
        require(std::abs(signal[i] - copy[i] * gain) < 1e-6, "Graphic EQ was not processed in the custom stage");
    p.eq.mode = "off"; p.effects.treble = 100;
    eq.configure(p, 48000, 2);
    float impulse[] = {1, 1, 0, 0};
    eq.process(impulse, 2);
    require(impulse[0] != 1 && impulse[0] == impulse[1], "Treble was not independently applied");
    bool rejected = false;
    try { eq.configure(p, 48000, 1); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Unsupported channel count was accepted");
}

void testCascadeAndBlocks() {
    Parameters p;
    p.eq.mode = "parametric";
    p.eq.preamp = -7;
    p.eq.filters = {{true, "PK", 1000, 5, 1.4}, {true, "LS", 150, -4, 0.7},
        {true, "HSC", 3000, 3, 0.7}, {true, "BP", 2500, -2, 2}};
    p.effects.treble = 30;
    for (const double frequency : {100.0, 1000.0, 6000.0}) {
        std::vector<float> signal(48000 * 2);
        double inputEnergy = 0;
        for (std::size_t i = 0; i < 48000; ++i) {
            const auto value = static_cast<float>(0.01 * std::sin(2 * 3.14159265358979323846 * frequency * i / 48000));
            signal[i * 2] = signal[i * 2 + 1] = value;
            if (i >= 24000) inputEnergy += value * value;
        }
        auto split = signal;
        ParametricEq wholeEq, splitEq;
        wholeEq.configure(p, 48000, 2);
        splitEq.configure(p, 48000, 2);
        wholeEq.process(signal.data(), 48000);
        for (std::size_t start = 0; start < 48000; start += 137)
            splitEq.process(split.data() + start * 2, std::min<std::size_t>(137, 48000 - start));
        require(signal == split, "Block boundaries changed the EQ output");
        double outputEnergy = 0;
        for (std::size_t i = 24000; i < 48000; ++i) outputEnergy += signal[i * 2] * signal[i * 2];
        double expected = p.eq.preamp;
        for (const auto& filter : p.eq.filters) expected += ParametricEq::responseDb(filter, frequency);
        expected += ParametricEq::responseDb({true, "HSC", 6000, 2.4, 0.7}, frequency);
        const double measured = 10 * std::log10(outputEnergy / inputEnergy);
        require(std::abs(measured - expected) <= 0.03, "Cascade process path did not match the reference response");
    }
    p.eq.preamp = 0; p.effects.treble = 0;
    p.eq.filters = {{false, "LP", 20, 0, 10}};
    ParametricEq eq;
    eq.configure(p, 48000, 2);
    float unchanged[] = {0.25f, -0.5f, 0.1f, 0.2f};
    const std::vector<float> original(std::begin(unchanged), std::end(unchanged));
    eq.process(unchanged, 2);
    require(std::equal(original.begin(), original.end(), std::begin(unchanged)), "Disabled filter processed samples");
    p.eq.preamp = 20; p.effects.treble = 100;
    p.eq.filters.assign(128, {true, "PK", 1000, 20, 10});
    eq.configure(p, 48000, 8);
    std::vector<float> stress(8192 * 8, 0.5f);
    stress[0] = std::numeric_limits<float>::quiet_NaN();
    stress[1] = std::numeric_limits<float>::infinity();
    eq.process(stress.data(), 8192);
    for (const auto value : stress) require(std::isfinite(value), "Maximum filter chain produced a non-finite sample");
}
}

void runEqTests() {
    readVectors();
    testParameters();
    testProcessing();
    testCascadeAndBlocks();
}

} // namespace hikari
