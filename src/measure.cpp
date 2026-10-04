// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "measure.h"
#include "dsp_adapter.h"
#include "parametric_eq.h"
#include "graphic_eq.h"
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <filesystem>
#include "security.h"

namespace hikari {
static Json measureAt(const Parameters& parameters, unsigned rate, const std::vector<double>& evaluation) {
    if (rate != 44100 && rate != 48000 && rate != 88200 && rate != 96000 && rate != 176400 && rate != 192000) throw std::invalid_argument("Unsupported measurement sample rate");
    DspTestRegistry isolation;
    DspAdapter adapter;
    adapter.apply(parameters);
    adapter.prepareFormat(rate, 2);
    constexpr std::size_t frames = 131072;
    constexpr std::size_t block = 256;
    constexpr double amplitude = 0.0001;
    std::vector<float> samples(frames * 2, 0);
    std::vector<float> warmup(block * 2, 0);
    for (unsigned i = 0; i < 32; ++i) {
        std::fill(warmup.begin(), warmup.end(), 0.0f);
        if (adapter.process(warmup.data(), static_cast<int>(block), 32, 2, rate, 32, 0) != 0) throw std::runtime_error("Measurement DSP warmup failed");
    }
    samples[0] = samples[1] = static_cast<float>(amplitude);
    for (std::size_t start = 0; start < frames; start += block)
        if (adapter.process(samples.data() + start * 2, static_cast<int>(block), 32, 2, rate, 32, 0) != 0) throw std::runtime_error("Measurement DSP processing failed");
    Json::Array frequencies, gains;
    constexpr double pi = 3.1415926535897932384626433832795;
    int latency = -1;
    for (std::size_t i = 0; i < frames; ++i) if (std::abs(samples[i * 2]) > amplitude * 1e-6) { latency = static_cast<int>(i); break; }
    for (const double frequency : evaluation) {
        const double angle = -2 * pi * frequency / rate;
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        std::complex<double> phase(1, 0), sum(0, 0);
        for (std::size_t n = 0; n < frames; ++n) {
            const double sample = samples[n * 2];
            if (!std::isfinite(sample)) throw std::runtime_error("Measurement output is non-finite");
            sum += phase * sample; phase *= step;
        }
        double gain = 20 * std::log10(std::max(std::abs(sum) / amplitude, 1e-15));
        frequencies.emplace_back(frequency); gains.emplace_back(gain);
    }
    return Json::object({{"ok", true}, {"sampleRate", rate}, {"channels", 2}, {"frequencies", frequencies}, {"gainDb", gains}, {"latencyFrames", latency}, {"impulseFrames", frames}, {"inputAmplitude", amplitude}});
}
Json measureResponse(const Parameters& parameters, unsigned rate) {
    std::vector<double> frequencies;
    const double maximum = std::min(20000.0, rate * 0.49);
    for (unsigned i = 0; i < 512; ++i) frequencies.push_back(20 * std::pow(maximum / 20, i / 511.0));
    return measureAt(parameters, rate, frequencies);
}
void runMeasureTests() {
    Parameters flat;
    auto measured = measureResponse(flat);
    double flatMaximum = 0;
    for (const auto& point : measured.at("gainDb").asArray()) {
        flatMaximum = std::max(flatMaximum, std::abs(point.asNumber()));
        if (std::abs(point.asNumber()) > 0.1) throw std::runtime_error("Full processing chain is not flat within 0.1 dB");
    }
    std::cout << "Full-chain flat max=" << flatMaximum << " dB\n";
    Parameters pk; pk.eq.mode = "parametric"; pk.eq.filters.push_back({true, "PK", 1000, 3, 1.4});
    measured = measureResponse(pk);
    const auto& frequencies = measured.at("frequencies").asArray(); const auto& gains = measured.at("gainDb").asArray();
    for (std::size_t i = 0; i < frequencies.size(); ++i) {
        const double expected = ParametricEq::responseDb(pk.eq.filters.front(), frequencies[i].asNumber());
        if (std::abs(gains[i].asNumber() - expected) > 0.1) throw std::runtime_error("Measured parametric processing differs from reference");
    }
    std::ifstream fixtureInput("tests/graphic-vectors.json", std::ios::binary);
    if (!fixtureInput) fixtureInput.open(std::filesystem::path(executablePath()).parent_path() / "test-vectors/graphic-vectors.json", std::ios::binary);
    if (!fixtureInput) throw std::runtime_error("Graphic measurement fixture unavailable");
    const std::string contents((std::istreambuf_iterator<char>(fixtureInput)), {});
    const auto fixture = Json::parse(contents);
    unsigned count = 0; double worst = 0;
    for (const auto& preset : fixture.at("presets").asArray()) {
        const bool enforce = preset.at("enforceThreshold").asBool();
        Parameters graphic; graphic.eq.mode = "graphic"; graphic.eq.points.clear();
        for (const auto& point : preset.at("points").asArray()) graphic.eq.points.push_back({point.at("freq").asNumber(), point.at("gain").asNumber()});
        std::vector<double> evaluation;
        for (const auto& frequency : preset.at("frequencies").asArray()) evaluation.push_back(frequency.asNumber());
        const auto response = measureAt(graphic, 48000, evaluation);
        const auto& actual = response.at("gainDb").asArray();
        const auto& expected = preset.at("expectedDb").asArray();
        double maximum = 0;
        for (std::size_t i = 0; i < actual.size(); ++i) maximum = std::max(maximum, std::abs(actual[i].asNumber() - expected[i].asNumber()));
        std::cout << "Full-chain graphic " << preset.at("key").asString() << ": max=" << maximum << " dB\n";
        if (enforce) {
            if (maximum > 1) throw std::runtime_error("Full-chain graphic fidelity exceeds 1 dB");
            worst = std::max(worst, maximum); ++count;
        }
    }
    if (count != 13) throw std::runtime_error("Full-chain graphic coverage incomplete");
    std::cout << "Full-chain graphic worst=" << worst << " dB; flat chain within 0.1 dB\n";
}
}
