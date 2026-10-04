// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "dsp_adapter.h"
#include "eq_stage.h"
#include "json.h"
#include "security.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hikari {
namespace {
constexpr unsigned kRate = 48000;
constexpr unsigned kChannels = 2;
// Not a divisor of the partition or fade length, so fades start and end inside blocks.
constexpr std::size_t kBlock = 441;
constexpr std::size_t kFrames = 2 * kRate;
constexpr std::size_t kSwitch = kRate;
constexpr double kStageLimit = 1e-6;
constexpr double kChainLimit = 1e-5;
constexpr std::uint64_t kLockCycleLimit = 2000000;

using Change = std::pair<std::size_t, Parameters>;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<float> makeSignal(const std::string& kind) {
    constexpr double pi = 3.14159265358979323846;
    std::vector<float> samples(kFrames * kChannels);
    unsigned state[kChannels] = {0x1234567u, 0x89abcdefu};
    const double frequency = kind == "100Hz" ? 100 : kind == "1kHz" ? 1000 : 6000;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        for (unsigned channel = 0; channel < kChannels; ++channel) {
            double value;
            if (kind == "noise") {
                state[channel] = state[channel] * 1664525u + 1013904223u;
                value = (static_cast<double>(state[channel]) / 4294967296.0 - 0.5) * 0.1;
            } else {
                value = 0.05 * std::sin(2 * pi * frequency * static_cast<double>(frame) / kRate + channel * 0.3);
            }
            samples[frame * kChannels + channel] = static_cast<float>(value);
        }
    }
    return samples;
}

Json loadFixture() {
    std::ifstream input("tests/graphic-vectors.json", std::ios::binary);
    if (!input) input.open(std::filesystem::path(executablePath()).parent_path() / "test-vectors/graphic-vectors.json", std::ios::binary);
    require(input.good(), "Transition fixture unavailable");
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    return Json::parse(text);
}

Parameters graphicPreset(const Json& fixture, const std::string& key) {
    for (const auto& preset : fixture.at("presets").asArray()) {
        if (preset.at("key").asString() != key) continue;
        Parameters parameters;
        parameters.eq.mode = "graphic";
        parameters.eq.points.clear();
        for (const auto& point : preset.at("points").asArray())
            parameters.eq.points.push_back({point.at("freq").asNumber(), point.at("gain").asNumber()});
        return clampParameters(parameters);
    }
    throw std::runtime_error("Missing transition preset: " + key);
}

Parameters parametric(bool second) {
    Parameters parameters;
    parameters.eq.mode = "parametric";
    if (second) {
        parameters.eq.preamp = 1;
        parameters.eq.filters = {{true, "LSC", 120, 5, 0.7}, {true, "PK", 3000, 4, 2}, {true, "PK", 6000, -6, 4}};
    } else {
        parameters.eq.preamp = -2;
        parameters.eq.filters = {{true, "PK", 100, 6, 1.4}, {true, "PK", 1000, -3, 1}, {true, "HSC", 8000, 4, 0.7}};
    }
    return clampParameters(parameters);
}

std::vector<double> fadeTable() {
    const auto resources = EqStage::allocate(kRate, kChannels);
    return resources.fade;
}

// Applies `changes` between blocks; block boundaries are split so that each change lands exactly on its frame.
template <class Apply, class Process>
std::vector<float> stream(const std::vector<float>& input, const std::vector<Change>& changes, Apply apply, Process process) {
    std::vector<float> output = input;
    std::size_t next = 0;
    for (std::size_t start = 0; start < kFrames;) {
        while (next < changes.size() && changes[next].first == start) apply(changes[next++].second);
        std::size_t count = std::min(kBlock, kFrames - start);
        if (next < changes.size()) count = std::min(count, changes[next].first - start);
        process(output.data() + start * kChannels, count);
        start += count;
    }
    return output;
}

std::vector<float> runStage(const std::vector<float>& input, const Parameters& initial, const std::vector<Change>& changes) {
    EqStage stage;
    auto resources = EqStage::allocate(kRate, kChannels);
    std::vector<std::unique_ptr<EqChain>> released;
    stage.reset(resources, EqStage::build(initial, kRate, kChannels), released);
    std::vector<float> scratch;
    return stream(input, changes, [&](const Parameters& next) {
        std::unique_ptr<EqChain> dropped;
        if (stage.retarget(next, dropped)) return;
        auto chain = EqStage::build(next, kRate, kChannels);
        require(stage.warm(*chain, scratch) && stage.synced(*chain), "Stage warm-up did not synchronize");
        stage.install(std::move(chain), dropped);
    }, [&](float* samples, std::size_t count) {
        stage.process(samples, count);
        stage.takeRetired(released);
        released.clear();
    });
}

std::vector<float> runSteady(const std::vector<float>& input, const Parameters& parameters, std::size_t from = 0) {
    ParametricEq filter;
    auto config = parameters;
    config.bypass = false;
    filter.configure(config, kRate, kChannels);
    std::vector<float> output = input;
    for (std::size_t start = from; start < kFrames; start += kBlock)
        filter.process(output.data() + start * kChannels, std::min(kBlock, kFrames - start));
    return output;
}

struct AdapterRun {
    std::vector<float> output;
    DspAdapter::Stats stats;
    std::vector<TransitionRecord> transitions;
};

AdapterRun runAdapter(const std::vector<float>& input, const Parameters& initial, const std::vector<Change>& changes) {
    DspAdapter adapter;
    adapter.apply(initial);
    adapter.prepareFormat(kRate, kChannels);
    AdapterRun run;
    run.output = stream(input, changes, [&](const Parameters& next) { adapter.apply(next); },
        [&](float* samples, std::size_t count) {
            require(adapter.process(samples, static_cast<int>(count), 32, kChannels, kRate, 32, 0) == 0, "Adapter transition processing failed");
            adapter.collect();
        });
    run.stats = adapter.stats();
    run.transitions = adapter.transitions();
    return run;
}

// positions[n] is the crossfade position applied at frame n: 0 renders `from`, total renders `to`.
std::vector<float> mixIdeal(const std::vector<float>& from, const std::vector<float>& to,
    const std::vector<std::size_t>& positions, const std::vector<double>& fade, std::size_t delay) {
    const std::size_t total = fade.size() - 1;
    std::vector<float> ideal(from.size());
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const std::size_t position = frame >= delay ? positions[frame - delay] : 0;
        for (unsigned channel = 0; channel < kChannels; ++channel) {
            const std::size_t index = frame * kChannels + channel;
            if (position == 0) ideal[index] = from[index];
            else if (position == total) ideal[index] = to[index];
            else ideal[index] = static_cast<float>(from[index] + fade[position] * (static_cast<double>(to[index]) - from[index]));
        }
    }
    return ideal;
}

std::vector<std::size_t> rampPositions(std::size_t start, std::size_t total) {
    std::vector<std::size_t> positions(kFrames, 0);
    for (std::size_t frame = start; frame < kFrames; ++frame) positions[frame] = std::min(frame - start + 1, total);
    return positions;
}

double maximumError(const std::vector<float>& actual, const std::vector<float>& expected) {
    double worst = 0;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        require(std::isfinite(actual[index]), "Transition produced a non-finite sample");
        worst = std::max(worst, std::abs(static_cast<double>(actual[index]) - expected[index]));
    }
    return worst;
}

// The upstream optimizer at zero effects is a fixed gain behind a look-ahead delay.
std::size_t chainDelay() {
    std::vector<float> impulse(kFrames * kChannels, 0);
    impulse[0] = impulse[1] = 0.01f;
    const auto output = runAdapter(impulse, Parameters{}, {}).output;
    std::size_t peak = 0;
    for (std::size_t frame = 0; frame < kRate; ++frame)
        if (std::abs(output[frame * kChannels]) > std::abs(output[peak * kChannels])) peak = frame;
    require(std::abs(output[peak * kChannels] - 0.01f) < 1e-6, "Unexpected zero-effect chain gain");
    return peak;
}

struct Totals {
    std::uint64_t lockCycles = 0, lockNanoseconds = 0, syncRetries = 0;
    void add(const DspAdapter::Stats& stats) {
        lockCycles = std::max(lockCycles, stats.applyLockMaxCycles);
        lockNanoseconds = std::max(lockNanoseconds, stats.applyLockMaxNanoseconds);
        syncRetries += stats.syncRetries;
    }
};

void testEqSwitches(const Json& fixture, const std::vector<double>& fade, std::size_t delay, Totals& totals) {
    const std::size_t total = fade.size() - 1;
    const auto first = graphicPreset(fixture, "1UBassBoost");
    const auto second = graphicPreset(fixture, "1UVoice");
    const auto peq = parametric(false);
    const auto peq2 = parametric(true);
    const struct { const char* name; Parameters from, to; } cases[] = {
        {"graphic-to-graphic", first, second},
        {"parametric-to-parametric", peq, peq2},
        {"graphic-to-parametric", first, peq2},
        {"parametric-to-graphic", peq2, first},
    };
    for (const auto& test : cases) {
        double worstNaive = 0;
        for (const char* signal : {"100Hz", "1kHz", "6kHz", "noise"}) {
            const auto input = makeSignal(signal);
            const std::vector<Change> changes{{kSwitch, test.to}};
            const auto positions = rampPositions(kSwitch, total);
            const auto stageFrom = runSteady(input, test.from);
            const auto stageTo = runSteady(input, test.to);
            const double stageError = maximumError(runStage(input, test.from, changes), mixIdeal(stageFrom, stageTo, positions, fade, 0));
            // The previous adapter replaced the chain with a cold one and switched at once.
            auto naive = stageFrom;
            const auto cold = runSteady(input, test.to, kSwitch);
            std::copy(cold.begin() + kSwitch * kChannels, cold.end(), naive.begin() + kSwitch * kChannels);
            const double naiveError = maximumError(naive, mixIdeal(stageFrom, stageTo, positions, fade, 0));
            worstNaive = std::max(worstNaive, naiveError);
            const auto run = runAdapter(input, test.from, changes);
            totals.add(run.stats);
            const auto chainFrom = runAdapter(input, test.from, {}).output;
            const auto chainTo = runAdapter(input, test.to, {}).output;
            const double chainError = maximumError(run.output, mixIdeal(chainFrom, chainTo, positions, fade, delay));
            std::cout << "Transition " << test.name << " " << signal << ": stage max=" << stageError
                      << " naive=" << naiveError << " chain max=" << chainError << std::endl;
            require(stageError <= kStageLimit, std::string("EQ crossfade deviates from the ideal mix: ") + test.name + " " + signal);
            require(chainError <= kChainLimit, std::string("Full-chain EQ crossfade deviates from the ideal mix: ") + test.name + " " + signal);
            require(run.transitions.size() == 1 && run.transitions[0].frame == kSwitch && run.transitions[0].kind == TransitionRecord::started,
                "EQ crossfade did not start at the applied frame");
            require(run.stats.chainsBuilt == 2, "EQ change did not build exactly one replacement chain");
        }
        // The metric must be able to see the click it is meant to exclude.
        require(worstNaive > 1000 * kStageLimit, std::string("Transition metric cannot detect a cold switch: ") + test.name);
    }
}

void testBypass(const Json& fixture, const std::vector<double>& fade, Totals& reported) {
    const std::size_t total = fade.size() - 1;
    auto processed = graphicPreset(fixture, "1UBassBoost");
    processed.effects.clarity = 4;
    processed.effects.surround = 2;
    auto bypassed = processed;
    bypassed.bypass = true;
    const std::size_t press = kSwitch, release = kSwitch + kRate / 2;
    for (const char* signal : {"100Hz", "1kHz", "6kHz", "noise"}) {
        const auto input = makeSignal(signal);
        const auto run = runAdapter(input, processed, {{press, bypassed}, {release, processed}});
        reported.add(run.stats);
        const auto wet = runAdapter(input, processed, {}).output;
        std::vector<std::size_t> positions(kFrames, 0);
        std::size_t position = 0;
        for (std::size_t frame = press; frame < kFrames; ++frame) {
            if (frame < release) position = std::min(position + 1, total);
            else if (position > 0) --position;
            positions[frame] = position;
        }
        const auto ideal = mixIdeal(wet, input, positions, fade, 0);
        const double error = maximumError(run.output, ideal);
        double naive = 0;
        for (std::size_t index = press * kChannels; index < (press + total) * kChannels; ++index)
            naive = std::max(naive, std::abs(static_cast<double>(input[index]) - ideal[index]));
        const bool dryHeld = std::memcmp(run.output.data() + (press + total) * kChannels, input.data() + (press + total) * kChannels,
            (release - press - total) * kChannels * sizeof(float)) == 0;
        std::cout << "Transition bypass " << signal << ": max=" << error << " naive=" << naive << std::endl;
        require(error <= kStageLimit, std::string("Bypass fade deviates from the ideal mix: ") + signal);
        require(dryHeld, "Settled bypass is not bit-identical to the input");
        require(run.stats.chainsBuilt == 1, "Bypass rebuilt an EQ chain");
        require(naive > 1000 * kStageLimit, "Bypass metric cannot detect an instant switch");
    }
}

void testIdentical(const Json& fixture) {
    auto parameters = graphicPreset(fixture, "1UVoice");
    parameters.effects.ambience = 3;
    for (const char* signal : {"100Hz", "1kHz", "6kHz", "noise"}) {
        const auto input = makeSignal(signal);
        const auto plain = runAdapter(input, parameters, {});
        auto copy = parameters;
        const auto repeated = runAdapter(input, parameters, {{kSwitch, copy}, {kSwitch + 1000, copy}});
        require(plain.output.size() == repeated.output.size() &&
            std::memcmp(plain.output.data(), repeated.output.data(), plain.output.size() * sizeof(float)) == 0,
            std::string("Identical apply changed the output: ") + signal);
        require(repeated.stats.chainsBuilt == plain.stats.chainsBuilt &&
            repeated.stats.upstreamSetterCalls == plain.stats.upstreamSetterCalls &&
            repeated.transitions.empty(), "Identical apply rebuilt the chain or called upstream setters");
        std::cout << "Transition identical " << signal << ": byte-identical, chains=" << repeated.stats.chainsBuilt
                  << " setters=" << repeated.stats.upstreamSetterCalls << std::endl;
    }
}

void testStateMachine(const Json& fixture, const std::vector<double>& fade) {
    const std::size_t total = fade.size() - 1;
    const auto first = graphicPreset(fixture, "1UBassBoost");
    const auto second = graphicPreset(fixture, "1UVoice");
    const auto third = parametric(true);
    const auto input = makeSignal("noise");
    const auto a = runSteady(input, first), b = runSteady(input, second), c = runSteady(input, third);
    // Turning back to the running chain mid-fade mirrors the position instead of restarting.
    const std::size_t turn = kSwitch + total / 3;
    std::vector<std::size_t> positions(kFrames, 0);
    for (std::size_t frame = kSwitch; frame < turn; ++frame) positions[frame] = frame - kSwitch + 1;
    std::size_t position = total - positions[turn - 1];
    std::vector<std::size_t> back(kFrames, 0);
    for (std::size_t frame = turn; frame < kFrames; ++frame) back[frame] = position = std::min(position + 1, total);
    auto expected = mixIdeal(a, b, positions, fade, 0);
    const auto returning = mixIdeal(b, a, back, fade, 0);
    std::copy(returning.begin() + turn * kChannels, returning.end(), expected.begin() + turn * kChannels);
    const double reverseError = maximumError(runStage(input, first, {{kSwitch, second}, {turn, first}}), expected);
    // A third target during a fade waits and starts when the running fade completes.
    const std::size_t queue = kSwitch + total / 4;
    const auto queuedFirst = mixIdeal(a, b, rampPositions(kSwitch, total), fade, 0);
    const auto queuedSecond = mixIdeal(b, c, rampPositions(kSwitch + total, total), fade, 0);
    auto queued = queuedFirst;
    std::copy(queuedSecond.begin() + (kSwitch + total) * kChannels, queuedSecond.end(), queued.begin() + (kSwitch + total) * kChannels);
    const double queueError = maximumError(runStage(input, first, {{kSwitch, second}, {queue, third}}), queued);
    // Replacing a queued target keeps only the newest one.
    const double replaceError = maximumError(runStage(input, first, {{kSwitch, second}, {queue, parametric(false)}, {queue + 10, third}}), queued);
    std::cout << "Transition state machine: reverse max=" << reverseError << " queued max=" << queueError
              << " replaced max=" << replaceError << std::endl;
    require(reverseError <= kStageLimit && queueError <= kStageLimit && replaceError <= kStageLimit,
        "EQ transition state machine deviates from the ideal mix");
}

// The retry path outside the lock: a warmed chain that fell behind catches up from the history and
// still lands exactly; one that fell out of the history reports it so the adapter rebuilds it.
void testCatchUp(const Json& fixture, const std::vector<double>& fade) {
    const std::size_t total = fade.size() - 1;
    const auto first = graphicPreset(fixture, "1UBassBoost");
    const auto second = graphicPreset(fixture, "1UVoice");
    const auto input = makeSignal("noise");
    const std::size_t behind = 1000, installAt = kSwitch + behind;
    EqStage stage;
    auto resources = EqStage::allocate(kRate, kChannels);
    std::vector<std::unique_ptr<EqChain>> released;
    stage.reset(resources, EqStage::build(first, kRate, kChannels), released);
    std::vector<float> scratch;
    auto output = input;
    std::unique_ptr<EqChain> chain;
    for (std::size_t start = 0; start < kFrames;) {
        if (start == kSwitch) {
            chain = EqStage::build(second, kRate, kChannels);
            require(stage.warm(*chain, scratch) && stage.synced(*chain), "Catch-up fixture warm-up failed");
        }
        if (start == installAt) {
            require(!stage.synced(*chain), "Catch-up fixture did not fall behind");
            require(stage.catchUp(*chain, scratch) && stage.synced(*chain), "Chain did not catch up from the history");
            std::unique_ptr<EqChain> dropped;
            stage.install(std::move(chain), dropped);
        }
        std::size_t count = std::min(kBlock, kFrames - start);
        if (start < kSwitch) count = std::min(count, kSwitch - start);
        else if (start < installAt) count = std::min(count, installAt - start);
        stage.process(output.data() + start * kChannels, count);
        stage.takeRetired(released);
        released.clear();
        start += count;
    }
    const double error = maximumError(output, mixIdeal(runSteady(input, first), runSteady(input, second), rampPositions(installAt, total), fade, 0));
    EqStage expired;
    auto expiredResources = EqStage::allocate(kRate, kChannels);
    expired.reset(expiredResources, EqStage::build(first, kRate, kChannels), released);
    auto stale = EqStage::build(second, kRate, kChannels);
    require(expired.warm(*stale, scratch), "Expired fixture warm-up failed");
    std::vector<float> filler(kBlock * kChannels, 0.01f);
    for (std::size_t written = 0; written <= InputHistory::kCapacityFrames; written += kBlock) expired.process(filler.data(), kBlock);
    const bool expiredRejected = !expired.catchUp(*stale, scratch) && !expired.synced(*stale);
    std::cout << "Transition catch-up: " << behind << " frames behind max=" << error
              << ", fell out of history rejected=" << (expiredRejected ? "yes" : "no") << std::endl;
    require(error <= kStageLimit, "Caught-up chain deviates from the ideal mix");
    require(expiredRejected, "A chain that fell out of the history was not reported");
}

void testConcurrent(const Json& fixture, const std::vector<double>& fade, std::size_t delay, Totals& totals) {
    const std::size_t total = fade.size() - 1;
    const Parameters presets[] = {graphicPreset(fixture, "1UBassBoost"), graphicPreset(fixture, "1UVoice")};
    const auto input = makeSignal("noise");
    DspAdapter adapter;
    adapter.apply(presets[0]);
    adapter.prepareFormat(kRate, kChannels);
    auto output = input;
    std::atomic<bool> failed{false};
    std::atomic<std::size_t> processed{0};
    std::thread audio([&] {
        for (std::size_t start = 0; start < kFrames; start += 480) {
            const auto count = static_cast<int>(std::min<std::size_t>(480, kFrames - start));
            if (adapter.process(output.data() + start * kChannels, count, 32, kChannels, kRate, 32, 0) != 0) failed = true;
            processed = start + count;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });
    unsigned applied = 0;
    for (unsigned round = 1; round <= 8 && processed < kFrames - kRate / 4; ++round) {
        while (adapter.completedTransitions() < applied && processed < kFrames) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        adapter.apply(presets[round % 2]);
        ++applied;
        adapter.collect();
    }
    audio.join();
    require(!failed, "Concurrent processing failed");
    totals.add(adapter.stats());
    const auto records = adapter.transitions();
    require(records.size() == applied && applied >= 2, "Concurrent transitions were not all recorded");
    const auto a = runAdapter(input, presets[0], {}).output;
    const auto b = runAdapter(input, presets[1], {}).output;
    // Alternating targets: build the expected stream fade by fade from the recorded start frames.
    std::vector<float> expected = a;
    for (std::size_t index = 0; index < records.size(); ++index) {
        require(records[index].kind == TransitionRecord::started, "Concurrent test expected plain fades only");
        const bool toSecond = index % 2 == 0;
        const auto mixed = mixIdeal(toSecond ? a : b, toSecond ? b : a, rampPositions(static_cast<std::size_t>(records[index].frame), total), fade, delay);
        const std::size_t from = static_cast<std::size_t>(records[index].frame) + delay;
        std::copy(mixed.begin() + from * kChannels, mixed.end(), expected.begin() + from * kChannels);
    }
    const double error = maximumError(output, expected);
    std::cout << "Transition concurrent: fades=" << records.size() << " max=" << error
              << " syncRetries=" << adapter.stats().syncRetries << std::endl;
    require(error <= kChainLimit, "Concurrent EQ crossfades deviate from the ideal mix");
}

void testEffectsLock(const Json& fixture, Totals& totals) {
    double lockMicroseconds[2] = {};
    for (unsigned changed : {5U, 1U}) {
        DspAdapter adapter;
        auto parameters = graphicPreset(fixture, "1UMusic");
        adapter.apply(parameters);
        adapter.prepareFormat(kRate, kChannels);
        std::vector<float> audio(kBlock * kChannels, 0.01f);
        require(adapter.process(audio.data(), static_cast<int>(kBlock), 32, kChannels, kRate, 32, 0) == 0, "Effects lock fixture failed");
        const auto before = adapter.stats();
        if (changed == 5) parameters.effects = {5, 4, 3, 2, 1, 0};
        else parameters.effects.bass = 7;
        adapter.apply(parameters);
        const auto after = adapter.stats();
        require(after.upstreamSetterCalls == before.upstreamSetterCalls + changed && after.chainsBuilt == before.chainsBuilt,
            "Effect-only apply must call each changed setter once and keep the EQ chain");
        adapter.apply(parameters);
        require(adapter.stats().upstreamSetterCalls == after.upstreamSetterCalls, "Unchanged effects were written again");
        lockMicroseconds[changed == 5 ? 0 : 1] = after.applyLockMaxNanoseconds / 1000.0;
        totals.add(after);
    }
    std::cout << "Transition effects-only apply: lock max=" << lockMicroseconds[0] << " us for five setters, "
              << lockMicroseconds[1] << " us for one (upstream setters run inside the lock)" << std::endl;
}

// Stage-level exactness for other rates and channel counts, on its own noise stream.
double stageSwitchError(const Parameters& from, const Parameters& to, unsigned rate, unsigned channels,
    std::size_t frames, std::size_t at, double* peak = nullptr, std::size_t* fadeLength = nullptr) {
    std::vector<float> input(frames * channels);
    unsigned state = 0x2468aceu;
    for (auto& value : input) {
        state = state * 1664525u + 1013904223u;
        value = static_cast<float>((static_cast<double>(state) / 4294967296.0 - 0.5) * 0.1);
    }
    const auto steady = [&](const Parameters& parameters) {
        ParametricEq filter;
        filter.configure(parameters, rate, channels);
        auto output = input;
        for (std::size_t start = 0; start < frames; start += kBlock)
            filter.process(output.data() + start * channels, std::min(kBlock, frames - start));
        return output;
    };
    const auto a = steady(from), b = steady(to);
    EqStage stage;
    auto resources = EqStage::allocate(rate, channels);
    const auto fade = resources.fade;
    std::vector<std::unique_ptr<EqChain>> released;
    stage.reset(resources, EqStage::build(from, rate, channels), released);
    std::vector<float> scratch;
    auto output = input;
    for (std::size_t start = 0; start < frames;) {
        if (start == at) {
            std::unique_ptr<EqChain> dropped;
            auto chain = EqStage::build(to, rate, channels);
            require(stage.warm(*chain, scratch) && stage.synced(*chain), "Format warm-up did not synchronize");
            stage.install(std::move(chain), dropped);
        }
        std::size_t count = std::min(kBlock, frames - start);
        if (start < at) count = std::min(count, at - start);
        stage.process(output.data() + start * channels, count);
        stage.takeRetired(released);
        released.clear();
        start += count;
    }
    const std::size_t total = fade.size() - 1;
    double worst = 0, loudest = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t position = frame < at ? 0 : std::min(frame - at + 1, total);
        for (unsigned channel = 0; channel < channels; ++channel) {
            const std::size_t index = frame * channels + channel;
            const float ideal = position == 0 ? a[index] : position == total ? b[index]
                : static_cast<float>(a[index] + fade[position] * (static_cast<double>(b[index]) - a[index]));
            require(std::isfinite(output[index]), "Format transition produced a non-finite sample");
            worst = std::max(worst, std::abs(static_cast<double>(output[index]) - ideal));
            loudest = std::max(loudest, std::abs(static_cast<double>(ideal)));
        }
    }
    if (peak) *peak = loudest;
    if (fadeLength) *fadeLength = total;
    return worst;
}

void testFormats(const Json& fixture) {
    const auto first = graphicPreset(fixture, "1UBassBoost");
    const auto second = graphicPreset(fixture, "1UVoice");
    std::size_t fade441 = 0;
    const double rate441 = stageSwitchError(first, parametric(true), 44100, 2, 2 * 44100, 44100, nullptr, &fade441);
    const double eight = stageSwitchError(first, second, 48000, 8, 2 * 48000, 48000);
    std::cout << "Transition formats: 44.1 kHz stereo graphic-to-parametric max=" << rate441 << " (fade " << fade441
              << " frames), 48 kHz 8-channel graphic-to-graphic max=" << eight << std::endl;
    require(fade441 == 882 && rate441 <= kStageLimit && eight <= kStageLimit, "EQ crossfade is not exact at another format");
    // Recorded only: a 20 Hz, Q 10 resonance rings longer than the 1.37 s history, so its warm start is approximate.
    Parameters ringing, ringing2;
    ringing.eq.mode = ringing2.eq.mode = "parametric";
    ringing.eq.filters = {{true, "PK", 20, 20, 10}};
    ringing2.eq.filters = {{true, "PK", 20, 14, 10}};
    double peak = 0;
    const double extreme = stageSwitchError(ringing, ringing2, 48000, 2, 4 * 48000, 3 * 48000, &peak);
    ParametricEq probe;
    probe.configure(ringing2, 48000, 2);
    std::cout << "Transition extreme low-frequency resonance (recorded, not gated): max=" << extreme << " of peak " << peak
              << ", warm-up wanted " << probe.warmupFrames(1u << 30) << " frames, capped at " << InputHistory::kReadableFrames << std::endl;
}
} // namespace

void runTransitionTests() {
    DspTestRegistry isolatedRegistry;
    const auto fixture = loadFixture();
    const auto fade = fadeTable();
    require(fade.size() == 961 && fade.front() == 0 && fade.back() == 1, "Unexpected 20 ms fade table");
    for (std::size_t position = 1; position < fade.size(); ++position)
        require(fade[position] > fade[position - 1] && std::abs(fade[position] + fade[fade.size() - 1 - position] - 1) < 1e-12,
            "Fade must rise monotonically and mirror around its midpoint");
    const std::size_t delay = chainDelay();
    Totals totals, bypassTotals;
    testEqSwitches(fixture, fade, delay, totals);
    testBypass(fixture, fade, bypassTotals);
    testIdentical(fixture);
    testStateMachine(fixture, fade);
    testCatchUp(fixture, fade);
    testConcurrent(fixture, fade, delay, totals);
    testEffectsLock(fixture, bypassTotals);
    testFormats(fixture);
    std::cout << "Transition summary: fade=20 ms (960 frames at 48 kHz), chain delay=" << delay
              << " frames, EQ apply lock max=" << totals.lockNanoseconds / 1000.0 << " us / " << totals.lockCycles
              << " cycles, bypass/effect apply lock max=" << bypassTotals.lockNanoseconds / 1000.0
              << " us, sync retries=" << totals.syncRetries << std::endl;
    require(totals.lockCycles <= kLockCycleLimit, "Heavy work ran while holding the audio lock");
}
} // namespace hikari
