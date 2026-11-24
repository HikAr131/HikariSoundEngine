// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "boom_compressor.h"
#include "delta_post.h"
#include "delta_status.h"
#include "dsp_adapter.h"
#include "footstep_booster.h"
#include "json.h"
#include "output_limiter.h"
#include "parameter_json.h"
#include "security.h"
#include "wind_constants.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kRate = 48000;

void require(bool value, const std::string& message) { if (!value) throw std::runtime_error("delta: " + message); }
void closeTo(double actual, double expected, double tolerance, const std::string& message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
        message + ": " + std::to_string(actual) + " instead of " + std::to_string(expected));
}
double db(double ratio) { return 20 * std::log10(std::max(ratio, 1e-12)); }

struct Noise {
    unsigned state;
    explicit Noise(unsigned seed) : state(seed) {}
    double next() { state = state * 1664525u + 1013904223u; return static_cast<double>(state) / 2147483648.0 - 1; }
};

Json golden() {
    std::ifstream input("tests/delta-golden.json", std::ios::binary);
    if (!input) input.open(std::filesystem::path(executablePath()).parent_path() / "test-vectors/delta-golden.json", std::ios::binary);
    require(input.good(), "golden table unavailable");
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    return Json::parse(text);
}

void testGolden() {
    const auto table = golden();
    const auto& w = table.at("wind");
    std::size_t compared = 0;
    const auto same = [&](const char* name, double value) {
        ++compared;
        require(w.at(name).asNumber() == value, std::string("constant differs from the golden table: ") + name);
    };
    using namespace wind;
    same("rate", wind::kRate); same("block", kBlock); same("window", kWindow); same("subFrame", kSubFrame); same("subFrames", kSubFrames);
    same("decimation", kDecimation); same("taps", kTaps); same("tukeyAlpha", kTukeyAlpha); same("fingerprintFft", kFingerprintFft);
    same("bands", kBands); same("bandLowHz", kBandLowHz); same("bandSpan", kBandSpan); same("powerFloor", kPowerFloor);
    same("normFloor", kNormFloor); same("coarseDecimation", kCoarseDecimation); same("rawTailStart", kRawTailStart); same("rawTail", kRawTail);
    same("presencePower", kPresencePower); same("presenceCosine", kPresenceCosine); same("searchInterval", kSearchInterval);
    same("maxPeaks", kMaxPeaks); same("peakDistance", kPeakDistance); same("coarseRadius", kCoarseRadius); same("coarseLags", kCoarseLags);
    same("fineRadius", kFineRadius); same("tryTwoScore", kTryTwoScore); same("alignScore", kAlignScore);
    same("looseCorrelation", kLooseCorrelation); same("wholeRadius", kWholeRadius);
    same("segment", kSegment); same("segmentRadius", kSegmentRadius); same("segmentSpan", kSegmentSpan); same("maxRateError", kMaxRateError);
    same("monoRatio", kMonoRatio); same("monoFloor", kMonoFloor); same("recentLockFrames", static_cast<double>(kRecentLockFrames)); same("fruitlessFrames", static_cast<double>(kFruitlessFrames)); same("heardPower", kHeardPower);
    same("recentLockDistance", kRecentLockDistance); same("maxSlots", kMaxSlots); same("handoverRadius", kHandoverRadius);
    same("handoverPerFrame", kHandoverPerFrame); same("localBack", kLocalBack); same("localPoints", kLocalPoints);
    same("confirmLow", kConfirmLow); same("confirmHigh", kConfirmHigh); same("confirmFrames", kConfirmFrames); same("confirmWarmup", kConfirmWarmup);
    same("confirmCorrelation", kConfirmCorrelation); same("confirmMargin", kConfirmMargin); same("confirmPasses", kConfirmPasses);
    same("trackRadius", kTrackRadius); same("trackFrames", kTrackFrames); same("trackEveryBlocks", kTrackEveryBlocks);
    same("trackFineSteps", kTrackFineSteps); same("sincTaps", kSincTaps); same("sincPhases", kSincPhases); same("sincBeta", kSincBeta);
    same("trackPeak", kTrackPeak); same("trackSidelobe", kTrackSidelobe);
    same("jumpOffset", kJumpOffset);
    same("resetOffset", kResetOffset); same("realignFrames", static_cast<double>(kRealignFrames));
    same("spectrumFft", kSpectrumFft); same("spectrumHop", kSpectrumHop); same("spectrumFrames", kSpectrumFrames); same("cutoffs", kCutoffs);
    same("cutoffLowHz", kCutoffLowHz); same("cutoffSpan", kCutoffSpan); same("noCutoffHz", kNoCutoffHz); same("bypassCutoffHz", kBypassCutoffHz);
    same("snapCutoffHz", kSnapCutoffHz); same("fitCoherence", kFitCoherence); same("fitPowerRatio", kFitPowerRatio); same("fitMinBins", kFitMinBins);
    same("fitBandCoherence", kFitBandCoherence); same("fitMinMagnitude", kFitMinMagnitude); same("fitMaxWeight", kFitMaxWeight);
    same("fitWeightFloor", kFitWeightFloor); same("fitMinBands", kFitMinBands); same("fitNoCutoffMargin", kFitNoCutoffMargin);
    same("fitMinGain", kFitMinGain); same("fitMaxGain", kFitMaxGain); same("fitMaxError", kFitMaxError); same("smoothing", kSmoothing);
    same("gainDownDb", kGainDownDb); same("gainUpDb", kGainUpDb); same("cutoffStepOctaves", kCutoffStepOctaves);
    same("presenceRing", kPresenceRing); same("presenceEffective", kPresenceEffective); same("presenceLow", kPresenceLow);
    same("presenceHigh", kPresenceHigh); same("presenceDown", kPresenceDown); same("presenceUp", kPresenceUp);
    same("energyBlocks", kEnergyBlocks); same("louderLimitDb", kLouderLimitDb); same("mismatchPower", kMismatchPower);
    same("mismatchRatio", kMismatchRatio); same("mismatchBands", kMismatchBands); same("recoverBlocks", std::round(1 / kRecoverStep));
    same("fadeInSeconds", kFadeInSeconds); same("fadeOutSeconds", kFadeOutSeconds); same("switchSeconds", kSwitchSeconds);
    same("qualityLowHz", kQualityLowHz); same("qualityHighHz", kQualityHighHz); same("qualityFloor", kQualityFloor); same("qualityBad", kQualityBad);
    same("qualityBlocks", kQualityBlocks); same("qualityMaxBad", kQualityMaxBad); same("qualityDrop", kQualityDrop); same("qualityReturn", kQualityReturn);
    same("forgetFrames", static_cast<double>(kForgetFrames));
    const auto list = [&](const char* name, const auto& values) {
        ++compared;
        const auto& expected = w.at(name).asArray();
        require(expected.size() == values.size(), std::string("golden list length: ") + name);
        for (std::size_t index = 0; index < values.size(); ++index)
            require(expected[index].asNumber() == static_cast<double>(values[index]), std::string("constant differs from the golden table: ") + name);
    };
    list("confirmLags", kConfirmLags); list("fitEdges", kFitEdges); list("crossovers", kCrossovers);
    ++compared;
    const auto& sections = w.at("trackFilter").asArray();
    require(sections.size() == kTrackFilter.size(), "golden track filter length");
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const double values[] = {kTrackFilter[index].b0, kTrackFilter[index].b1, kTrackFilter[index].b2, kTrackFilter[index].a1, kTrackFilter[index].a2};
        for (std::size_t item = 0; item < 5; ++item) require(sections[index].asArray()[item].asNumber() == values[item], "golden track filter");
    }
    require(compared == w.asObject().size(), "golden table holds values nothing compares");

    const auto& f = table.at("footstep");
    const auto& gains = f.at("gainDb").asArray();
    require(gains.size() == 11, "footstep table length");
    for (int level = 0; level <= 10; ++level) closeTo(footstepGainDb(level), gains[static_cast<std::size_t>(level)].asNumber(), 1e-9, "footstep gain table");
    require(f.at("lookaheadSeconds").asNumber() == FootstepBooster::kLookaheadSeconds && f.at("ceiling").asNumber() == FootstepBooster::kCeiling &&
        f.at("levelCutoffHz").asNumber() == FootstepBooster::kLevelCutoffHz && f.at("levelTarget").asNumber() == FootstepBooster::kLevelTarget &&
        f.at("minimumBoost").asNumber() == FootstepBooster::kMinimumBoost && f.at("loudPeakDb").asNumber() == FootstepBooster::kLoudPeakDb &&
        f.at("attackSeconds").asNumber() == FootstepBooster::kAttackSeconds && f.at("releaseSeconds").asNumber() == FootstepBooster::kReleaseSeconds &&
        f.at("highPassHz").asNumber() == FootstepBooster::kHighPassHz && f.at("highPassQ").asNumber() == FootstepBooster::kHighPassQ, "footstep constants");
    const auto& b = table.at("boom");
    for (const auto level : {BoomLevel::light, BoomLevel::medium, BoomLevel::strong}) {
        const auto curve = BoomCompressor::curve(level);
        const auto& expected = b.at(boomName(level)).asArray();
        require(expected[0].asNumber() == curve.thresholdDb && expected[1].asNumber() == curve.ratio && expected[2].asNumber() == curve.kneeDb, "boom curve");
    }
    require(b.at("lookaheadSeconds").asNumber() == BoomCompressor::kLookaheadSeconds && b.at("holdSeconds").asNumber() == BoomCompressor::kHoldSeconds &&
        b.at("attackSeconds").asNumber() == BoomCompressor::kAttackSeconds && b.at("fastReleaseSeconds").asNumber() == BoomCompressor::kFastReleaseSeconds &&
        b.at("slowReleaseSeconds").asNumber() == BoomCompressor::kSlowReleaseSeconds && b.at("slowAboveDb").asNumber() == BoomCompressor::kSlowAboveDb &&
        b.at("fastBelowDb").asNumber() == BoomCompressor::kFastBelowDb && b.at("modeHoldSeconds").asNumber() == BoomCompressor::kModeHoldSeconds, "boom constants");
    const auto& l = table.at("limiter");
    require(l.at("ceilingDb").asNumber() == OutputLimiter::kCeilingDb && l.at("lookaheadSeconds").asNumber() == OutputLimiter::kLookaheadSeconds &&
        l.at("heavyDb").asNumber() == OutputLimiter::kHeavyDb && l.at("windowSeconds").asNumber() == OutputLimiter::kWindowSeconds &&
        l.at("warnAbove").asNumber() == OutputLimiter::kWarnAbove && l.at("clearBelow").asNumber() == OutputLimiter::kClearBelow, "limiter constants");
}

template <class Stage>
std::vector<float> runStage(Stage& stage, std::vector<float> samples, unsigned channels) {
    for (std::size_t frame = 0; frame < samples.size() / channels; ++frame) stage.processFrame(samples.data() + frame * channels);
    return samples;
}
std::vector<float> sine(double frequency, double amplitude, double seconds, unsigned channels, double rate = kRate) {
    std::vector<float> samples(static_cast<std::size_t>(seconds * rate) * channels);
    for (std::size_t frame = 0; frame < samples.size() / channels; ++frame) for (unsigned channel = 0; channel < channels; ++channel)
        samples[frame * channels + channel] = static_cast<float>(amplitude * std::sin(2 * kPi * frequency * static_cast<double>(frame) / rate + channel * 0.4));
    return samples;
}
double rms(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    double sum = 0;
    for (std::size_t index = from; index < to; ++index) sum += static_cast<double>(samples[index]) * samples[index];
    return std::sqrt(sum / static_cast<double>(to - from));
}
double tailGainDb(const std::vector<float>& input, const std::vector<float>& output, double seconds, unsigned channels, double rate) {
    const std::size_t count = static_cast<std::size_t>(seconds * rate) * channels;
    return db(rms(output, output.size() - count, output.size()) / rms(input, input.size() - count, input.size()));
}

void testFootstep() {
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        for (int level = 1; level <= 10; ++level) {
            FootstepBooster stage;
            stage.prepare(rate, 2);
            stage.setGainDb(footstepGainDb(level));
            stage.reset();
            const auto input = sine(1000, 0.002, 0.6, 2, rate);
            closeTo(tailGainDb(input, runStage(stage, input, 2), 0.2, 2, rate), footstepGainDb(level), 0.2, "footstep small-signal gain");
        }
    }
    const auto loud = [](double peakDb) {
        FootstepBooster stage;
        stage.prepare(kRate, 2);
        stage.setGainDb(footstepGainDb(10));
        stage.reset();
        // A quiet bed with one loud burst: the burst itself has to pass at unity.
        auto input = sine(1000, 0.002, 1.5, 2);
        const double amplitude = std::pow(10.0, peakDb / 20);
        const std::size_t from = 48000, to = 48000 + 9600;
        for (std::size_t frame = from; frame < to; ++frame) for (unsigned channel = 0; channel < 2; ++channel)
            input[frame * 2 + channel] = static_cast<float>(amplitude * std::sin(2 * kPi * 1000 * static_cast<double>(frame) / kRate));
        const auto output = runStage(stage, input, 2);
        return db(rms(output, (from + 2400) * 2, (to - 2400) * 2) / rms(input, (from + 2400) * 2, (to - 2400) * 2));
    };
    closeTo(loud(-8.5), 0, 0.15, "loud block boosted");
    closeTo(loud(-3), 0, 0.15, "loud block boosted");
    require(loud(-12) > 3, "block below the loud threshold is no longer boosted");
    // The boosted level is capped: 0.1 RMS times 3.8 would exceed 0.32, so the gain settles at 0.32 / 0.1.
    {
        FootstepBooster stage;
        stage.prepare(kRate, 2);
        stage.setGainDb(footstepGainDb(10));
        stage.reset();
        const auto input = sine(1000, 0.1 * std::sqrt(2.0), 14, 2);
        closeTo(tailGainDb(input, runStage(stage, input, 2), 1, 2, kRate), db(3.2), 0.3, "footstep level cap");
    }
    for (const double frequency : {30.0, 60.0, 120.0, 1000.0}) {
        FootstepBooster stage;
        stage.prepare(kRate, 2);
        stage.setGainDb(footstepGainDb(5));
        stage.reset();
        const auto input = sine(frequency, 0.002, 1.5, 2);
        const double gain = tailGainDb(input, runStage(stage, input, 2), 0.5, 2, kRate) - footstepGainDb(5);
        const double ratio = frequency / 60, expected = 20 * std::log10(ratio * ratio / std::sqrt(std::pow(1 - ratio * ratio, 2) + std::pow(ratio / 0.707, 2)));
        closeTo(gain, expected, 0.25, "footstep high-pass");
    }
    // Off is an exact delay.
    FootstepBooster off;
    off.prepare(kRate, 2);
    off.setGainDb(0);
    off.reset();
    Noise noise(5);
    std::vector<float> input(9600);
    for (auto& sample : input) sample = static_cast<float>(noise.next() * 0.5);
    const auto output = runStage(off, input, 2);
    for (std::size_t index = off.latencyFrames() * 2; index < input.size(); ++index)
        require(output[index] == input[index - off.latencyFrames() * 2], "footstep off is not an exact delay");
    // Never above the ceiling while boosting.
    FootstepBooster hot;
    hot.prepare(kRate, 2);
    hot.setGainDb(footstepGainDb(10));
    hot.reset();
    for (auto& sample : input) sample = static_cast<float>(noise.next() * 0.35);
    for (const float sample : runStage(hot, input, 2)) require(std::abs(sample) < 1.2f, "footstep output runs away");
}

void testBoom() {
    for (const double rate : {44100.0, 48000.0}) for (const auto level : {BoomLevel::light, BoomLevel::medium, BoomLevel::strong}) {
        for (double levelDb = -36; levelDb <= 0; levelDb += 3) {
            BoomCompressor stage;
            stage.prepare(rate, 2);
            stage.setLevel(level);
            stage.reset();
            const auto input = sine(1000, std::pow(10.0, levelDb / 20), 0.8, 2, rate);
            const auto curve = BoomCompressor::curve(level);
            const double over = levelDb - curve.thresholdDb;
            double expected = over <= -curve.kneeDb / 2 ? 0 : over >= curve.kneeDb / 2 ? (1 - 1 / curve.ratio) * over
                : (1 - 1 / curve.ratio) * std::pow(over + curve.kneeDb / 2, 2) / (2 * curve.kneeDb);
            closeTo(-tailGainDb(input, runStage(stage, input, 2), 0.2, 2, rate), expected, 0.2, std::string("boom static curve ") + boomName(level));
        }
    }
    {
        BoomCompressor stage;
        stage.prepare(kRate, 2);
        stage.setLevel(BoomLevel::off);
        stage.reset();
        const auto input = sine(1000, 0.95, 0.2, 2);
        const auto output = runStage(stage, input, 2);
        for (std::size_t index = stage.latencyFrames() * 2; index < input.size(); ++index)
            require(output[index] == input[index - stage.latencyFrames() * 2], "boom off is not a true bypass");
    }
    // Constant signals give direct access to the gain: attack and both release speeds.
    const auto gains = [](double loudSeconds, double tailSeconds) {
        BoomCompressor stage;
        stage.prepare(kRate, 2);
        stage.setLevel(BoomLevel::strong);
        stage.reset();
        const auto loud = static_cast<std::size_t>(loudSeconds * kRate), quiet = static_cast<std::size_t>(0.1 * kRate);
        const auto tail = static_cast<std::size_t>(tailSeconds * kRate);
        std::vector<float> samples((quiet + loud + tail) * 2, 0.01f);
        std::fill(samples.begin() + static_cast<std::ptrdiff_t>(quiet * 2), samples.begin() + static_cast<std::ptrdiff_t>((quiet + loud) * 2), 1.0f);
        const auto output = runStage(stage, samples, 2);
        std::vector<double> reduction;
        for (std::size_t frame = quiet; frame < quiet + loud + tail; ++frame) {
            const std::size_t source = frame - stage.latencyFrames();
            reduction.push_back(-db(output[frame * 2] / samples[source * 2]));
        }
        return reduction;
    };
    const double full = BoomCompressor::reductionDb(BoomCompressor::curve(BoomLevel::strong), 0);
    {
        const auto reduction = gains(0.05, 0.3);
        // The burst leaves 2 ms after it was seen, twenty attack times later.
        closeTo(reduction[96], full, 0.05, "boom reduction when the burst leaves");
        closeTo(reduction[4], full * (1 - std::exp(-5 / 4.8)), full * 0.08, "boom attack");
        const std::size_t release = static_cast<std::size_t>((0.05 + 0.012) * kRate);
        closeTo(reduction[release + 960], full * std::exp(-1.0), full * 0.05, "boom fast release");
    }
    {
        const auto reduction = gains(0.5, 1.0);
        const std::size_t release = static_cast<std::size_t>((0.5 + 0.012) * kRate);
        closeTo(reduction[release + 12000], full * std::exp(-1.0), full * 0.05, "boom slow release after a long burst");
    }
    {
        BoomCompressor stage;
        stage.prepare(kRate, 2);
        stage.setLevel(BoomLevel::strong);
        stage.reset();
        Noise noise(11);
        bool slow = false;
        for (std::size_t frame = 0; frame < 96000; ++frame) {
            const double phase = static_cast<double>(frame % 4800) / kRate;
            const auto value = static_cast<float>(noise.next() * std::exp(-phase / 0.06));
            float samples[2] = {value, value};
            stage.processFrame(samples);
            slow = slow || stage.slowRelease();
        }
        require(slow, "automatic fire does not slow the release down");
        bool fast = false;
        for (std::size_t frame = 0; frame < 144000; ++frame) {
            float samples[2] = {0.001f, 0.001f};
            stage.processFrame(samples);
            fast = !stage.slowRelease();
        }
        require(fast, "release does not become quick again");
    }
}

// Reference true peak: 16 times oversampled with a much longer kernel than the limiter uses.
double truePeak(const std::vector<float>& samples, unsigned channels) {
    constexpr int factor = 16, half = 48;
    const auto frames = static_cast<std::ptrdiff_t>(samples.size() / channels);
    double peak = 0;
    for (unsigned channel = 0; channel < channels; ++channel) for (std::ptrdiff_t frame = half; frame + half < frames; ++frame) {
        peak = std::max(peak, static_cast<double>(std::abs(samples[static_cast<std::size_t>(frame) * channels + channel])));
        for (int step = 1; step < factor; ++step) {
            const double fraction = static_cast<double>(step) / factor;
            double sum = 0;
            for (int tap = -half + 1; tap <= half; ++tap) {
                const double offset = tap - fraction;
                const double window = 0.42 + 0.5 * std::cos(kPi * offset / half) + 0.08 * std::cos(2 * kPi * offset / half);
                sum += samples[static_cast<std::size_t>(frame + tap) * channels + channel] * std::sin(kPi * offset) / (kPi * offset) * window;
            }
            peak = std::max(peak, std::abs(sum));
        }
    }
    return peak;
}

void testLimiter() {
    for (const unsigned channels : {2u, 8u}) for (const double rate : {44100.0, 48000.0}) {
        for (const std::string kind : {"noise", "soft", "shots", "tone", "quarter", "steps"}) {
            OutputLimiter stage;
            stage.prepare(rate, channels);
            Noise noise(99);
            std::vector<float> input(static_cast<std::size_t>(rate * 0.4) * channels);
            double low = 0, lower = 0;
            for (std::size_t frame = 0; frame < input.size() / channels; ++frame) for (unsigned channel = 0; channel < channels; ++channel) {
                const double t = static_cast<double>(frame) / rate, n = noise.next();
                low += 0.35 * (n - low);
                lower += 0.35 * (low - lower);
                double value = 0;
                if (kind == "noise") value = low * 5;
                if (kind == "soft") value = lower * 8;
                if (kind == "shots") value = low * 6 * std::exp(-std::fmod(t, 0.1) / 0.02);
                if (kind == "tone") value = 1.8 * std::sin(2 * kPi * 440 * t + channel);
                if (kind == "quarter") value = 1.4 * std::sin(2 * kPi * rate / 4 * t + kPi / 4);
                if (kind == "steps") value = std::sin(2 * kPi * 60 * t) > 0 ? 1.3 : -1.3;
                input[frame * channels + channel] = static_cast<float>(value);
            }
            const auto output = runStage(stage, input, channels);
            const float ceiling = static_cast<float>(std::pow(10.0, -1.0 / 20));
            for (const float sample : output) require(std::abs(sample) <= ceiling, "limiter exceeds -1 dBFS");
            // White noise and raw steps at 13 dB of limiting are no audio; everything else has to hold between samples too.
            if (channels == 2 && kind != "steps" && kind != "noise") {
                const double peak = db(truePeak(output, channels));
                require(peak <= -0.9, "limiter true peak above -0.9 dBFS for " + kind + ": " + std::to_string(peak));
                require(peak >= -2.5, "limiter takes away far more than it has to for " + kind + ": " + std::to_string(peak));
            }
        }
    }
    // Below the ceiling it is an exact delay.
    OutputLimiter quiet;
    quiet.prepare(kRate, 2);
    const auto calm = sine(1000, 0.5, 0.2, 2);
    const auto passed = runStage(quiet, calm, 2);
    for (std::size_t index = quiet.latencyFrames() * 2; index < calm.size(); ++index)
        require(passed[index] == calm[index - quiet.latencyFrames() * 2], "limiter touches audio below its ceiling");
    // The warning follows the share of heavily limited frames with hysteresis.
    OutputLimiter stage;
    stage.prepare(kRate, 2);
    const auto feed = [&](double amplitude, double seconds) {
        for (std::size_t frame = 0; frame < static_cast<std::size_t>(seconds * kRate); ++frame) {
            float samples[2] = {static_cast<float>(amplitude), static_cast<float>(amplitude)};
            stage.processFrame(samples);
        }
    };
    feed(0.5, 1);
    require(!stage.warning(), "limiter warns without limiting");
    feed(3, 0.09);
    require(!stage.warning(), "limiter warns below a fifth of the window");
    feed(3, 0.03);
    require(stage.warning(), "limiter does not warn above a fifth of the window");
    feed(0.1, 0.38);
    feed(3, 0.045);
    feed(0.1, 0.04);
    require(stage.warning(), "limiter warning clears above a tenth of the window");
    feed(0.1, 0.6);
    require(!stage.warning(), "limiter warning does not clear");
}

Parameters deltaParameters(double footstep, BoomLevel boom) {
    Parameters parameters;
    parameters.eq.mode = "parametric";
    parameters.delta.enabled = true;
    parameters.delta.wind = false;
    parameters.delta.footstep = footstep;
    parameters.delta.boom = boom;
    return parameters;
}

void processAll(DspAdapter& adapter, std::vector<float>& samples, int rate, int channels, std::size_t block = 480) {
    for (std::size_t frame = 0; frame < samples.size() / channels; frame += block) {
        const auto count = static_cast<int>(std::min(block, samples.size() / channels - frame));
        require(adapter.process(samples.data() + frame * channels, count, 32, channels, rate, 32, 0) == 0, "adapter processing failed");
    }
}

void testAdapter() {
    for (const int rate : {44100, 48000, 96000}) for (const int channels : {2, 4, 6, 8}) {
        DspAdapter adapter;
        adapter.apply(deltaParameters(0, BoomLevel::off));
        adapter.prepareFormat(rate, channels);
        Noise noise(21);
        std::vector<float> input(static_cast<std::size_t>(rate / 2) * channels);
        for (auto& sample : input) sample = static_cast<float>(noise.next() * 0.3);
        auto output = input;
        const auto blocks = adapter.stats().upstreamBlocks;
        processAll(adapter, output, rate, channels);
        require(adapter.stats().upstreamBlocks == blocks, "the profile still runs the upstream chain");
        const std::size_t delay = adapter.deltaLatency().total() * static_cast<std::size_t>(channels);
        for (std::size_t index = delay; index < input.size(); ++index)
            require(output[index] == input[index - delay], "the neutral profile is not an exact delay");
        const double milliseconds = 1000.0 * static_cast<double>(adapter.deltaLatency().total()) / rate;
        require(milliseconds <= 12, "the profile adds more than 12 ms: " + std::to_string(milliseconds));
        if (rate == 48000 && channels == 2) std::cout << "Delta added latency " << milliseconds << " ms, replacing "
            << adapter.deltaLatency().upstream << " upstream frames\n";
    }
    // Off leaves every sample of the previous chain alone.
    {
        Parameters plain;
        plain.eq.mode = "parametric";
        plain.eq.filters = {{true, "PK", 500, 4, 1.4}};
        plain.effects = {3, 0, 2, 4, 3, 0};
        auto withDelta = plain;
        withDelta.delta = {false, true, 10, BoomLevel::strong};
        Noise noise(31);
        std::vector<float> input(48000);
        for (auto& sample : input) sample = static_cast<float>(noise.next() * 0.2);
        auto first = input, second = input;
        DspAdapter a, b;
        a.apply(plain); a.prepareFormat(48000, 2);
        b.apply(withDelta); b.prepareFormat(48000, 2);
        processAll(a, first, 48000, 2);
        processAll(b, second, 48000, 2);
        require(first == second, "a disabled profile changes the sound");
    }
    // No step anywhere while the profile, its stages and the equalizer are switched around.
    {
        DspAdapter adapter;
        auto parameters = deltaParameters(5, BoomLevel::light);
        parameters.delta.enabled = false;
        adapter.apply(parameters);
        adapter.prepareFormat(48000, 2);
        std::vector<float> samples = sine(220, 0.08, 6, 2);
        const double steady = 0.08 * std::pow(2 * kPi * 220 / kRate, 2);
        std::size_t frame = 0;
        const auto run = [&](double seconds) {
            const auto end = frame + static_cast<std::size_t>(seconds * kRate);
            for (; frame < end; frame += 441) require(adapter.process(samples.data() + frame * 2, 441, 32, 2, 48000, 32, 0) == 0, "processing failed");
        };
        run(0.5);
        parameters.delta.enabled = true; adapter.apply(parameters); run(0.5);
        parameters.delta.footstep = 10; adapter.apply(parameters); run(0.4);
        parameters.delta.boom = BoomLevel::strong; adapter.apply(parameters); run(0.4);
        parameters.eq.filters = {{true, "PK", 220, 6, 1.4}}; adapter.apply(parameters); run(0.4);
        parameters.bypass = true; adapter.apply(parameters); run(0.05);
        parameters.bypass = false; adapter.apply(parameters); run(0.4);
        parameters.delta.footstep = 0; adapter.apply(parameters); run(0.4);
        parameters.delta.boom = BoomLevel::off; adapter.apply(parameters); run(0.4);
        parameters.delta.enabled = false; adapter.apply(parameters); run(0.05);
        parameters.delta.enabled = true; adapter.apply(parameters); run(0.05);
        parameters.delta.enabled = false; adapter.apply(parameters); run(0.6);
        double worst = 0;
        for (std::size_t at = 9602; at < frame * 2; ++at)
            worst = std::max(worst, std::abs(static_cast<double>(samples[at]) - 2.0 * samples[at - 2] + samples[at - 4]));
        // Four times louder after all boosts, and room for the fades themselves.
        require(worst < steady * 30, "switching the profile clicks: " + std::to_string(worst / steady));
    }
    // Bypass returns the untouched input, with the profile on as well.
    {
        DspAdapter adapter;
        auto parameters = deltaParameters(10, BoomLevel::strong);
        adapter.apply(parameters);
        adapter.prepareFormat(48000, 2);
        auto warm = sine(300, 0.2, 0.3, 2);
        processAll(adapter, warm, 48000, 2);
        parameters.bypass = true;
        adapter.apply(parameters);
        processAll(adapter, warm, 48000, 2);
        auto samples = sine(300, 0.2, 0.2, 2);
        const auto original = samples;
        processAll(adapter, samples, 48000, 2);
        require(samples == original, "bypass with the profile on changed audio");
    }
    // Non-finite input never gets through.
    {
        DspAdapter adapter;
        adapter.apply(deltaParameters(10, BoomLevel::strong));
        adapter.prepareFormat(48000, 2);
        std::vector<float> samples(9600, 0.2f);
        samples[500] = std::numeric_limits<float>::quiet_NaN();
        samples[900] = std::numeric_limits<float>::infinity();
        processAll(adapter, samples, 48000, 2);
        for (const float sample : samples) require(std::isfinite(sample) && std::abs(sample) <= 0.9f, "non-finite input reaches the output");
    }
    // A missing recording file is reported and changes nothing audible.
    {
        DspAdapter adapter(std::filesystem::path("absent") / WindReferenceSet::kFileName);
        auto parameters = deltaParameters(0, BoomLevel::off);
        parameters.delta.wind = true;
        adapter.apply(parameters);
        adapter.prepareFormat(48000, 2);
        auto samples = sine(300, 0.2, 0.2, 2);
        const auto original = samples;
        processAll(adapter, samples, 48000, 2);
        const std::size_t delay = adapter.deltaLatency().total() * 2;
        for (std::size_t index = delay; index < samples.size(); ++index) require(samples[index] == original[index - delay], "an unavailable wind stage changes audio");
        bool reported = false;
        for (int attempt = 0; attempt < 200 && !reported; ++attempt) {
            reported = adapter.deltaStatus().wind.state == WindState::refsInvalid;
            if (!reported) Sleep(10);
        }
        require(reported, "missing recordings are not reported");
        parameters.delta.wind = false;
        adapter.apply(parameters);
        require(adapter.deltaStatus().wind.state == WindState::off, "a wind stage switched off is still reported");
        parameters.delta.wind = true;
        parameters.delta.enabled = false;
        adapter.apply(parameters);
        require(adapter.deltaStatus().wind.state == WindState::paused, "a paused wind stage is not reported");
        adapter.collect();
    }
}

void testParameters() {
    auto parsed = parseParameters(Json::parse(R"({"delta":{"enabled":true,"wind":false,"footstep":7.4,"boom":"strong","other":1}})"));
    require(parsed.delta.enabled && !parsed.delta.wind && parsed.delta.footstep == 7 && parsed.delta.boom == BoomLevel::strong, "delta parameters");
    require(parseParameters(Json::parse(R"({"delta":{"footstep":99}})")).delta.footstep == 10, "footstep clamp");
    require(parseParameters(Json::parse(R"({"delta":{"footstep":-1}})")).delta.footstep == 0, "footstep clamp");
    const auto absent = parseParameters(Json::parse("{}")).delta;
    require(!absent.enabled && absent.wind && absent.footstep == 5 && absent.boom == BoomLevel::light, "delta defaults");
    for (const char* text : {R"({"delta":{"boom":"loud"}})", R"({"delta":{"boom":1}})", R"({"delta":[]})", R"({"delta":{"enabled":1}})", R"({"delta":{"footstep":"5"}})"}) {
        bool rejected = false;
        try { parseParameters(Json::parse(text)); } catch (const JsonError&) { rejected = true; }
        require(rejected, std::string("accepted ") + text);
    }
    const auto round = parseParameters(parametersJson(parsed));
    require(sameParameters(round, parsed), "delta parameter round trip");
    auto other = parsed;
    other.delta.boom = BoomLevel::off;
    require(!sameParameters(other, parsed) && sameEqualizer(other, parsed), "a delta change must not rebuild the equalizer");
    const auto restored = restoredParameters(persistedParametersJson(parsed));
    require(sameDelta(restored.delta, parsed.delta), "delta survives a restart");
}

void testStatus() {
    DeltaSnapshot snapshot;
    snapshot.wind = {WindState::locked, 1, 0.876, 5432.1, -12.34};
    const auto json = snapshot.json();
    require(json.at("wind").at("state").asString() == "locked" && json.at("wind").at("reference").asNumber() == 2 &&
        json.at("footstep").at("state").asString() == "off" && !json.at("limiterWarning").asBool(), "delta status shape");
    closeTo(json.at("wind").at("quality").asNumber(), 0.88, 1e-9, "status quality");
    snapshot.wind = {};
    require(snapshot.json().at("wind").at("reference").isNull() && snapshot.json().at("wind").at("state").asString() == "off", "idle delta status");
    Parameters parameters;
    require(DeltaSnapshot::idle(parameters).json().at("wind").at("state").asString() == "paused", "wind wanted but the profile is off");
    parameters.delta.wind = false;
    require(DeltaSnapshot::idle(parameters).json().at("wind").at("state").asString() == "off", "wind off");

    DeltaEventGate gate;
    DeltaSnapshot current;
    current.wind.state = WindState::searching;
    require(gate.filter(current, 5000).wind.state == WindState::searching, "the first state is shown at once");
    current.wind.state = WindState::locked;
    current.wind.quality = 0.5;
    require(gate.filter(current, 5400).wind.state == WindState::searching, "state events come faster than once a second");
    require(gate.filter(current, 6000).wind.state == WindState::locked, "a changed state is not shown");
    current.wind.quality = 0.58;
    require(gate.filter(current, 9000).wind.quality == 0.5, "a small quality change is pushed");
    current.wind.quality = 0.62;
    require(gate.filter(current, 9100).wind.quality == 0.62, "a clear quality change is not pushed");
    current.wind.cutoff = 777;
    require(gate.filter(current, 20000).wind.cutoff == 0, "diagnostic values alone cause a push");
    current.limiterWarning = true;
    require(gate.filter(current, 20001).limiterWarning, "the limiter warning is not pushed");
}
}

void runDeltaTests() {
    DspTestRegistry isolation;
    testGolden();
    testParameters();
    testStatus();
    testFootstep();
    testBoom();
    testLimiter();
    testAdapter();
}
} // namespace hikari
