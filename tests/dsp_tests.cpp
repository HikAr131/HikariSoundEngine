// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "dsp_adapter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace hikari {
void runDspTests() {
    DspTestRegistry isolatedRegistry;
    bool flatFailed = false;
    const auto effectAudio = [](int effect) {
        DspAdapter adapter;
        Parameters parameters;
        if (effect >= 0) {
            double* values[] = {&parameters.effects.clarity, &parameters.effects.ambience,
                &parameters.effects.surround, &parameters.effects.dynamicBoost, &parameters.effects.bass};
            *values[effect] = 10;
        }
        adapter.apply(parameters);
        adapter.prepareFormat(48000, 2);
        std::vector<float> output;
        output.reserve(32768);
        constexpr double pi = 3.14159265358979323846;
        for (int block = 0; block < 16; ++block) {
            std::vector<float> audio(2048);
            for (int frame = 0; frame < 1024; ++frame) {
                const double time = static_cast<double>(block * 1024 + frame) / 48000;
                audio[frame * 2] = static_cast<float>(0.03 * (std::sin(2*pi*30*time) + std::sin(2*pi*300*time) + std::sin(2*pi*7000*time)));
                audio[frame * 2 + 1] = static_cast<float>(0.03 * (std::sin(2*pi*80*time) + std::sin(2*pi*500*time) + std::sin(2*pi*4000*time)));
            }
            if (adapter.process(audio.data(), 1024, 32, 2, 48000, 32, 0) != 0)
                throw std::runtime_error("DSP effect audio comparison failed");
            output.insert(output.end(), audio.begin(), audio.end());
        }
        return output;
    };
    const auto withoutEffects = effectAudio(-1);
    for (int effect = 0; effect < 5; ++effect) {
        const auto withEffect = effectAudio(effect);
        double difference = 0;
        for (std::size_t sample = 0; sample < withEffect.size(); ++sample) {
            if (!std::isfinite(withEffect[sample])) throw std::runtime_error("DSP effect produced non-finite audio");
            const double delta = withEffect[sample] - withoutEffects[sample];
            difference += delta * delta;
        }
        if (difference <= 1e-8) throw std::runtime_error("DSP effect did not change actual audio: " + std::to_string(effect));
    }
    for (const char* mode : {"off", "graphic"}) {
        for (double frequency : {30.0, 100.0, 1000.0, 10000.0, 16000.0}) {
            DspAdapter flat;
            Parameters parameters;
            parameters.eq.mode = mode;
            parameters.eq.points = {{20, 0}, {20000, 0}};
            flat.apply(parameters);
            flat.prepareFormat(48000, 2);
            double inputEnergy = 0, outputEnergy = 0;
            constexpr int frames = 1024;
            constexpr double pi = 3.14159265358979323846;
            for (int block = 0; block < 96; ++block) {
                std::vector<float> audio(frames * 2);
                for (int frame = 0; frame < frames; ++frame) {
                    const auto sample = static_cast<float>(0.05 * std::sin(2 * pi * frequency * (block * frames + frame) / 48000));
                    audio[frame * 2] = sample;
                    audio[frame * 2 + 1] = sample;
                    if (block >= 48) inputEnergy += sample * sample * 2;
                }
                if (flat.process(audio.data(), frames, 32, 2, 48000, 32, 0) != 0)
                    throw std::runtime_error("Zero-effects flat processing failed");
                if (block >= 48) for (float sample : audio) outputEnergy += sample * sample;
            }
            const double gainDb = 10 * std::log10(outputEnergy / inputEnergy);
            std::fprintf(stderr, "Flat %s %.0f gain %.9f dB\n", mode, frequency, gainDb);
            if (!std::isfinite(gainDb) || std::abs(gainDb) > 0.1) flatFailed = true;
        }
    }
    for (int rate : {44100, 48000, 96000, 192000}) {
        for (int channels : {2, 4, 6, 8}) {
            DspAdapter adapter;
            Parameters parameters;
            parameters.eq.mode = "graphic";
            parameters.effects = {4, 1, 2, 5, 3, 0};
            adapter.apply(parameters);
            adapter.prepareFormat(rate, channels);
            const float effects[] = {4, 1, 2, 5, 3};
            for (int effect = 0; effect < 5; ++effect) {
                if (std::abs(adapter.upstream()->getEffectValue(static_cast<DfxDsp::Effect>(effect)) - effects[effect] / 10) > 0.0001f)
                    throw std::runtime_error("DSP effect scale/readback failed");
            }
            std::vector<float> audio(512 * channels, 0.01f);
            if (adapter.process(audio.data(), 512, 32, channels, rate, 32, 0) != 0)
                throw std::runtime_error("DSP format initialization/process failed");
            for (float sample : audio) if (!std::isfinite(sample))
                throw std::runtime_error("DSP produced non-finite audio");
            parameters.eq.points = {{20, -4}, {1000, 2}, {20000, -2}};
            adapter.apply(parameters);
            for (int block = 0; block < 4; ++block) {
                std::fill(audio.begin(), audio.end(), 0.01f);
                if (adapter.process(audio.data(), 512, 32, channels, rate, 32, 0) != 0)
                    throw std::runtime_error("Prepared DSP graphic update failed");
                for (float sample : audio) if (!std::isfinite(sample))
                    throw std::runtime_error("Prepared DSP graphic update produced non-finite audio");
            }
            const auto beforeFormatChange = audio;
            const int otherRate = rate == 48000 ? 44100 : 48000;
            if (adapter.process(audio.data(), 512, 32, channels, otherRate, 32, 0) == 0 || audio != beforeFormatChange)
                throw std::runtime_error("DSP accepted an unprepared format change");
        }
    }
    for (int channels : {2, 4, 6, 8}) {
        DspAdapter adapter;
        Parameters maximum;
        maximum.eq.mode = "graphic";
        maximum.eq.preamp = 12;
        maximum.effects = {10, 10, 10, 10, 10, 100};
        maximum.eq.points = {{20, 12}, {20000, 12}};
        adapter.apply(maximum);
        std::vector<float> unprepared(1024 * channels, 0.05f);
        const auto beforePreparation = unprepared;
        if (adapter.process(unprepared.data(), 1024, 32, channels, 48000, 32, 0) == 0 || unprepared != beforePreparation)
            throw std::runtime_error("DSP accepted an unprepared audio format");
        adapter.prepareFormat(48000, channels);
        unsigned noise = 0x1a2b3c4d;
        for (int block = 0; block < 200; ++block) {
            std::vector<float> audio(1024 * channels);
            for (float& sample : audio) {
                noise = noise * 1664525u + 1013904223u;
                sample = (noise & 0x80000000u) ? 1.0f : -1.0f;
            }
            if (adapter.process(audio.data(), 1024, 32, channels, 48000, 32, 0) != 0)
                throw std::runtime_error("DSP full-scale processing failed");
            for (float sample : audio) if (!std::isfinite(sample) || std::abs(sample) > 1.0f)
                throw std::runtime_error("DSP limiter exceeded full scale or produced NaN: " + std::to_string(channels) + " channels");
        }
        maximum.bypass = true;
        adapter.apply(maximum);
        std::vector<float> bypass(512 * channels, 0.5f);
        const auto original = bypass;
        if (adapter.process(bypass.data(), 512, 32, channels, 48000, 32, 0) != 0 || bypass != original)
            throw std::runtime_error("DSP bypass changed audio");
    }
    for (int channels : {2, 4, 6, 8}) {
        for (double level : {0.001, 1.0}) {
            DspAdapter adapter;
            Parameters maximum;
            maximum.eq.mode = "parametric";
            maximum.eq.preamp = 20;
            maximum.eq.filters.assign(kMaxFilters, Filter{true, "PK", 1000, 20, 10});
            maximum.effects = {10, 10, 10, 10, 10, 100};
            adapter.apply(maximum);
            adapter.prepareFormat(48000, channels);
            constexpr double pi = 3.14159265358979323846;
            for (int block = 0; block < 100; ++block) {
                std::vector<float> audio(1024 * channels);
                for (int frame = 0; frame < 1024; ++frame) {
                    const auto sample = static_cast<float>(level * std::sin(2*pi*1000*(block*1024+frame)/48000));
                    for (int channel = 0; channel < channels; ++channel) audio[frame*channels+channel] = sample;
                }
                const int result = adapter.process(audio.data(), 1024, 32, channels, 48000, 32, 0);
                if (result != 0) throw std::runtime_error("Maximum parametric chain returned " + std::to_string(result));
                for (std::size_t index = 0; index < audio.size(); ++index) {
                    if (!std::isfinite(audio[index]) || std::abs(audio[index]) > 1.0f)
                        throw std::runtime_error("Maximum parametric chain violated finite/full-scale output: channels=" +
                            std::to_string(channels) + " level=" + std::to_string(level) + " block=" +
                            std::to_string(block) + " sample=" + std::to_string(index) + " value=" + std::to_string(audio[index]));
                }
            }
        }
    }
    if (flatFailed) throw std::runtime_error("Zero-effects processing chain is not flat");
}
} // namespace hikari
