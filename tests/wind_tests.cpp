// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "capture_clock.h"
#include "resampler.h"
#include "security.h"
#include "sha256.h"
#include "wind_fingerprint.h"
#include "wind_stage.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
using Samples = std::vector<std::int16_t>;

void require(bool value, const std::string& message) { if (!value) throw std::runtime_error("wind: " + message); }

struct Noise {
    unsigned long long state;
    explicit Noise(unsigned long long seed) : state(seed * 2862933555777941757ull + 3037000493ull) {}
    double uniform() {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return static_cast<double>(state >> 11) / 9007199254740992.0;
    }
    double gauss() { return std::sqrt(-2 * std::log(uniform() + 1e-300)) * std::cos(2 * kPi * uniform()); }
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    static Biquad lowpass(double cutoff, double rate) {
        Biquad q;
        if (cutoff >= 23999) return q;
        const double w = 2 * kPi * cutoff / rate, c = std::cos(w), alpha = std::sin(w) / std::sqrt(2.0), a0 = 1 + alpha;
        q.b0 = q.b2 = (1 - c) / (2 * a0); q.b1 = (1 - c) / a0; q.a1 = -2 * c / a0; q.a2 = (1 - alpha) / a0;
        return q;
    }
    static Biquad peak(double frequency, double gain, double width, double rate) {
        Biquad q;
        const double a = std::pow(10.0, gain / 40), w = 2 * kPi * frequency / rate, c = std::cos(w), alpha = std::sin(w) / (2 * width), a0 = 1 + alpha / a;
        q.b0 = (1 + alpha * a) / a0; q.b1 = q.a1 = -2 * c / a0; q.b2 = (1 - alpha * a) / a0; q.a2 = (1 - alpha / a) / a0;
        return q;
    }
};

// A stand-in for the published recordings: gusty, low-heavy, decorrelated stereo noise.
Samples syntheticWind(unsigned seed, std::size_t frames) {
    Noise noise(seed);
    Samples samples(frames * 2);
    Biquad shape[2] = {Biquad::lowpass(900, 48000), Biquad::lowpass(900, 48000)}, wide[2] = {Biquad::lowpass(3500, 48000), Biquad::lowpass(3500, 48000)};
    double gust = 0, target = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (frame % 9600 == 0) target = noise.gauss() * 0.5;
        gust += (target - gust) * 2e-4;
        const double common = noise.gauss(), level = 0.06 * std::exp(gust);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const double own = noise.gauss(), value = shape[channel].run(0.5 * common + own) + 0.12 * wide[channel].run(own) + 0.01 * own;
            samples[frame * 2 + channel] = static_cast<std::int16_t>(std::lround(std::clamp(value * level, -0.95, 0.95) * 32767));
        }
    }
    return samples;
}

// Interpolation independent of the engine's own table.
double at(const Samples& samples, double position, std::size_t channel) {
    const auto length = static_cast<long long>(samples.size() / 2);
    position = std::fmod(position, static_cast<double>(length));
    if (position < 0) position += static_cast<double>(length);
    const auto whole = static_cast<long long>(std::floor(position));
    const double fraction = position - static_cast<double>(whole);
    if (fraction < 1e-9) return samples[static_cast<std::size_t>(whole) * 2 + channel] / 32768.0;
    double sum = 0;
    for (int tap = -31; tap <= 32; ++tap) {
        const double offset = tap - fraction, window = 0.42 + 0.5 * std::cos(kPi * offset / 32.5) + 0.08 * std::cos(2 * kPi * offset / 32.5);
        const long long index = ((whole + tap) % length + length) % length;
        sum += samples[static_cast<std::size_t>(index) * 2 + channel] / 32768.0 * std::sin(kPi * offset) / (kPi * offset) * window;
    }
    return sum;
}

struct Scenario {
    std::size_t reference = 0;
    double position = 500000.37, gainDb = -6, cutoff = 24000, ppm = 0, eqDb = 0, footsteps = 0.02, voice = 0, shots = 0, seconds = 8;
    unsigned rate = 48000, seed = 1;
    double stopAt = 0, gapAt = 0;
    bool gapKnown = true;
    // The tracker gets no time, and every block may lose a few frames, from `stallAt` for `stallSeconds`.
    double stallAt = 0, stallSeconds = 0;
    std::size_t stallGap = 0;
};
struct Outcome {
    double lockSeconds = -1, residualDb = 0, footstepDb = 0, worstStepDb = 0, recoverSeconds = -1, silentSeconds = -1, worstSample = 0, windPeak = 0;
    double tailDb = 0;
    int steps = 0;
    std::uint64_t losses = 0;
    int reference = -1;
};

Outcome play(const Scenario& s, const std::vector<Samples>& raw, const std::shared_ptr<const WindReferenceSet>& references) {
    const std::size_t frames = static_cast<std::size_t>(s.seconds * s.rate), block = s.rate / 100;
    std::vector<float> wind(frames * 2), steps(frames * 2), other(frames * 2), out(frames * 2);
    Noise noise(s.seed);
    Biquad low[2], eq[2], voice[2], foot[2], shot[2];
    for (std::size_t channel = 0; channel < 2; ++channel) {
        low[channel] = Biquad::lowpass(s.cutoff, s.rate); eq[channel] = Biquad::peak(1200, s.eqDb, 1, s.rate);
        voice[channel] = Biquad::lowpass(2500, s.rate); foot[channel] = Biquad::lowpass(700, s.rate); shot[channel] = Biquad::lowpass(7000, s.rate);
    }
    const double amplitude = std::pow(10.0, s.gainDb / 20), step = 48000.0 / s.rate * (1 + s.ppm * 1e-6);
    const auto& source = raw[s.reference];
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / s.rate;
        const bool blowing = s.stopAt <= 0 || t < s.stopAt;
        const double speaking = (std::sin(2 * kPi * 0.4 * t) > 0 ? 1 : 0) * (0.5 + 0.5 * std::sin(2 * kPi * 4.3 * t)), v = noise.gauss();
        const double since = std::fmod(t, 0.45), stepLevel = since < 0.12 ? std::exp(-since / 0.02) : 0, a = noise.gauss(), b = noise.gauss();
        const double shotLevel = std::fmod(t, 4.0) < 2 ? std::exp(-std::fmod(t, 1.3) / 0.05) : 0, c = noise.gauss(), d = noise.gauss();
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const std::size_t index = frame * 2 + channel;
            wind[index] = blowing ? static_cast<float>(eq[channel].run(low[channel].run(at(source, s.position + step * static_cast<double>(frame), channel))) * amplitude) : 0.0f;
            steps[index] = static_cast<float>(s.footsteps * 4 * foot[channel].run(0.7 * a + 0.3 * (channel ? b : -b)) * stepLevel);
            other[index] = static_cast<float>(steps[index] + s.voice * voice[channel].run(v) * speaking + s.shots * shot[channel].run(0.8 * c + 0.2 * (channel ? d : -d)) * shotLevel);
            out[index] = wind[index] + other[index];
        }
    }
    WindStage stage(s.rate, 2, references);
    Outcome outcome;
    const std::size_t gapStart = s.gapAt > 0 ? static_cast<std::size_t>(s.gapAt * 100) * block : 0, gapLength = 5 * block;
    bool gapped = false;
    const auto residual = [&](std::size_t from, std::size_t to) {
        double error = 0, power = 1e-20;
        for (std::size_t index = from * 2; index < to * 2; ++index) {
            error += std::pow(static_cast<double>(out[index]) - other[index], 2);
            power += static_cast<double>(wind[index]) * wind[index];
        }
        return 10 * std::log10(error / power + 1e-30);
    };
    for (std::size_t frame = 0; frame + block <= frames; frame += block) {
        if (gapStart && !gapped && frame >= gapStart) {
            gapped = true;
            frame += gapLength;
            stage.gap(s.gapKnown ? static_cast<std::int64_t>(gapLength) : -1);
        }
        const bool stalled = s.stallAt > 0 && frame >= static_cast<std::size_t>(s.stallAt * s.rate) && frame < static_cast<std::size_t>((s.stallAt + s.stallSeconds) * s.rate);
        if (stalled && s.stallGap && frame + s.stallGap + block <= frames) {
            frame += s.stallGap;
            stage.gap(static_cast<std::int64_t>(s.stallGap));
        }
        stage.process(out.data() + frame * 2, block);
        if (!stalled) stage.pump();
        const double t = static_cast<double>(frame + block) / s.rate;
        const bool locked = stage.status().state == WindState::locked;
        if (locked && outcome.lockSeconds < 0) { outcome.lockSeconds = t; outcome.reference = stage.status().reference; }
        if (gapped && outcome.recoverSeconds < 0 && t > s.gapAt + 0.15 && residual(frame + block - s.rate / 20, frame + block) < -13) outcome.recoverSeconds = t - s.gapAt;
        if (s.stopAt > 0 && t > s.stopAt && outcome.silentSeconds < 0 && stage.canceller().appliedGain() <= 0) outcome.silentSeconds = t - s.stopAt;
    }
    outcome.losses = stage.tracker()->losses();
    outcome.tailDb = residual(frames - s.rate, frames);
    if (outcome.lockSeconds < 0) return outcome;
    const auto from = static_cast<std::size_t>((outcome.lockSeconds + 0.8) * s.rate), to = s.stopAt > 0 ? static_cast<std::size_t>(s.stopAt * s.rate) : gapStart ? gapStart : frames;
    outcome.residualDb = residual(from, to);
    double product = 0, square = 0;
    for (std::size_t index = from * 2; index < to * 2; ++index) {
        product += static_cast<double>(out[index]) * steps[index];
        square += static_cast<double>(steps[index]) * steps[index];
    }
    if (square > 0) outcome.footstepDb = 20 * std::log10(std::max(1e-6, product / square));
    // Every single step on its own: an average would hide one that is gone.
    const auto period = static_cast<std::size_t>(0.45 * s.rate), length = static_cast<std::size_t>(0.06 * s.rate);
    for (std::size_t step = (from / period + 1) * period; step + length < to; step += period) {
        double along = 0, own = 0;
        for (std::size_t index = step * 2; index < (step + length) * 2; ++index) {
            along += static_cast<double>(out[index]) * steps[index];
            own += static_cast<double>(steps[index]) * steps[index];
        }
        if (own <= 0) continue;
        const double change = 20 * std::log10(std::max(1e-6, along / own));
        if (std::abs(change) > std::abs(outcome.worstStepDb)) outcome.worstStepDb = change;
        ++outcome.steps;
    }
    for (std::size_t index = 0; index < frames * 2; ++index) outcome.windPeak = std::max(outcome.windPeak, std::abs(static_cast<double>(wind[index])));
    const auto after = s.stopAt > 0 ? static_cast<std::size_t>(s.stopAt * s.rate) : from;
    for (std::size_t index = after * 2; index < frames * 2; ++index) {
        if (gapStart && index >= gapStart * 2 && index < (gapStart + gapLength) * 2) continue;
        outcome.worstSample = std::max(outcome.worstSample, std::abs(static_cast<double>(out[index]) - other[index]));
    }
    return outcome;
}

void testBasics() {
    Sha256 hash;
    hash.update("abc", 3);
    require(Sha256::hex(hash.finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 vector");
    Sha256 split;
    const std::string text(1000, 'a');
    for (std::size_t at = 0; at < text.size(); at += 7) split.update(text.data() + at, std::min<std::size_t>(7, text.size() - at));
    Sha256 whole;
    whole.update(text.data(), text.size());
    require(Sha256::hex(split.finish()) == Sha256::hex(whole.finish()), "SHA-256 in pieces");

    // Straight from the definition: box decimation, Tukey window, 256-point DFT, overlap-weighted log bands.
    Noise noise(3);
    std::vector<float> frame(wind::kSubFrame * 2);
    for (auto& sample : frame) sample = static_cast<float>(noise.gauss() * 0.05);
    const WindFingerprint fingerprint;
    const auto energies = fingerprint.analyze(frame.data());
    std::array<double, 129> power{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t bin = 0; bin < power.size(); ++bin) {
            std::complex<double> sum;
            for (std::size_t index = 0; index < 160; ++index) {
                double box = 0;
                for (std::size_t sub = 0; sub < 6; ++sub) box += frame[(index * 6 + sub) * 2 + channel];
                const double u = static_cast<double>(std::min(index, 159 - index)) / 159.0;
                const double window = u < 0.125 ? 0.5 * (1 + std::cos(kPi * (8 * u - 1))) : 1;
                sum += box / 6 * window * std::polar(1.0, -2 * kPi * static_cast<double>(bin * index) / 256);
            }
            power[bin] += std::norm(sum);
        }
    }
    for (std::size_t band = 0; band < 16; ++band) {
        const double low = 100 * std::pow(32.0, static_cast<double>(band) / 16), high = 100 * std::pow(32.0, static_cast<double>(band + 1) / 16);
        double sum = 0;
        for (std::size_t bin = 0; bin < power.size(); ++bin)
            sum += std::max(0.0, std::min((static_cast<double>(bin) + 0.5) * 31.25, high) - std::max((static_cast<double>(bin) - 0.5) * 31.25, low)) / 31.25 * power[bin];
        require(std::abs(energies[band] - std::log(std::max(1e-30, sum))) < 1e-4, "fingerprint differs from its definition");
    }

    const WindInterpolator interpolator;
    for (std::size_t phase = 0; phase <= wind::kSincPhases; phase += 64) {
        double sum = 0;
        for (std::size_t tap = 0; tap < wind::kSincTaps; ++tap) sum += interpolator.phase(phase)[tap];
        require(std::abs(sum - 1) < 1e-5, "interpolation phases are not normalized");
    }

    for (const unsigned rate : {22050u, 44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        const RationalResampler resampler(rate, 48000);
        for (const double frequency : {100.0, 1000.0, 6000.0}) {
            double error = 0, total = 0;
            for (std::int64_t output = 400; output < 3000; ++output) {
                const std::int64_t first = resampler.inputFrame(output) - resampler.half() + 1;
                const float* taps = resampler.coefficients(output);
                double sum = 0;
                for (std::size_t tap = 0; tap < resampler.taps(); ++tap) sum += taps[tap] * std::sin(2 * kPi * frequency * static_cast<double>(first + static_cast<std::int64_t>(tap)) / rate);
                const double expected = std::sin(2 * kPi * frequency * static_cast<double>(output) / 48000);
                error += (sum - expected) * (sum - expected);
                total += expected * expected;
            }
            require(10 * std::log10(error / total) < -60, "resampler error at " + std::to_string(rate));
        }
    }
}

struct TemporaryFile {
    std::filesystem::path path;
    explicit TemporaryFile(const std::vector<unsigned char>& bytes) {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() / ("hikari-wind-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter) + ".bin");
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ~TemporaryFile() { std::error_code code; std::filesystem::remove(path, code); }
};

void testLoading() {
    const auto samples = syntheticWind(77, 96000);
    Sha256 hash;
    for (const auto sample : samples) { const float value = static_cast<float>(sample) / 32768.0f; hash.update(&value, sizeof(value)); }
    const auto digest = hash.finish();
    const std::vector<std::string> digests = {Sha256::hex(digest)};
    std::vector<unsigned char> bytes = {'H', 'K', 'W', 'R'};
    const auto put = [&](unsigned long long value, int count) { for (int index = 0; index < count; ++index) bytes.push_back(static_cast<unsigned char>(value >> (8 * index))); };
    put(1, 4); put(1, 4); put(48000, 4); put(2, 4); put(96000, 8);
    bytes.insert(bytes.end(), digest.begin(), digest.end());
    const std::size_t data = bytes.size();
    bytes.resize(data + samples.size() * 2);
    std::memcpy(bytes.data() + data, samples.data(), samples.size() * 2);
    const auto load = [&](const std::vector<unsigned char>& content, const std::vector<std::string>& expected) {
        TemporaryFile file(content);
        return WindReferenceSet::load(file.path, expected);
    };
    const auto good = load(bytes, digests);
    require(good && good->items().size() == 1 && good->items()[0].frames == 96000 && good->items()[0].samples == samples, "a valid recording file is refused");
    require(!load(bytes, WindReferenceSet::publishedDigests()), "a recording that is not the published one is accepted");
    for (const std::size_t flip : {std::size_t(0), std::size_t(4), std::size_t(8), std::size_t(12), std::size_t(16), std::size_t(20), std::size_t(30), data + 5}) {
        auto broken = bytes;
        broken[flip] ^= 1;
        require(!load(broken, digests), "an altered recording file is accepted at " + std::to_string(flip));
    }
    auto longer = bytes;
    longer.push_back(0);
    require(!load(longer, digests), "trailing bytes are accepted");
    auto shorter = bytes;
    shorter.pop_back();
    require(!load(shorter, digests), "a truncated recording file is accepted");
    require(!load({}, digests) && !WindReferenceSet::load("absent.bin", digests), "a missing recording file is accepted");
    // A file another program holds open is told from one that is wrong, and giving up early works.
    {
        TemporaryFile file(bytes);
        bool unreadable = true;
        require(WindReferenceSet::load(file.path, digests, &unreadable) && !unreadable, "a readable recording file is called unreadable");
        require(!WindReferenceSet::load(file.path, WindReferenceSet::publishedDigests(), &unreadable) && !unreadable, "a wrong recording file is called unreadable");
        require(!WindReferenceSet::load("absent.bin", digests, &unreadable) && !unreadable, "a missing recording file is called unreadable");
        const std::atomic<bool> stop{true};
        require(!WindReferenceSet::load(file.path, digests, nullptr, &stop), "loading does not give up");
        const HANDLE held = CreateFileW(file.path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(held != INVALID_HANDLE_VALUE, "the test could not hold the file");
        const bool refused = !WindReferenceSet::load(file.path, digests, &unreadable);
        CloseHandle(held);
        require(refused && unreadable, "a recording file that is held open is called wrong");
    }
    // The hops behind the end follow the loop, also when it ends inside a sub-frame.
    {
        const auto odd = syntheticWind(5, 96000 + 512);
        const auto set = WindReferenceSet::fromSamples({odd});
        const auto& reference = set->items()[0];
        const WindFingerprint fingerprint;
        std::vector<float> frame(wind::kSubFrame * 2);
        const auto energies = [&](std::size_t hop) {
            for (std::size_t index = 0; index < frame.size(); ++index) frame[index] = static_cast<float>(odd[(hop * wind::kSubFrame * 2 + index) % odd.size()]) / 32768.0f;
            return fingerprint.analyze(frame.data());
        };
        const auto first = energies(0);
        for (const std::size_t hop : {std::size_t(1), reference.hops - 1, reference.hops, reference.hops + 1, reference.stride() - 1}) {
            const auto expected = energies(hop);
            for (std::size_t band = 0; band < wind::kBands; ++band)
                require(std::abs((reference.band(band)[hop] - reference.band(band)[0]) - (expected[band] - first[band])) < 1e-4, "the fingerprint breaks at the joint of the loop");
        }
    }
    // Packets: only a loss Windows reports is sized, and a pause or a new thread leaves nothing to go by.
    {
        CaptureClock clock;
        clock.packet(0, 480, 0, 1000, 10);
        require(clock.count() == 1 && clock.gap(0).frames == -1, "the first packet of a thread claims to continue");
        clock.clear();
        clock.packet(0, 480, 0, 1480, 20);
        clock.packet(480, 480, 0, 1960, 30);
        require(clock.count() == 0, "adjoining packets report a gap");
        clock.packet(960, 480, CaptureClock::kDiscontinuity, 2440 + 700, 40);
        require(clock.count() == 1 && clock.gap(0).offset == 960 && clock.gap(0).frames == 700, "a reported loss is not sized");
        clock.packet(1440, 480, CaptureClock::kDiscontinuity | CaptureClock::kTimestampError, 9000, 50);
        require(clock.count() == 2 && clock.gap(1).frames == -1, "a position that cannot be trusted is used");
        clock.clear();
        clock.packet(0, 480, 0, 9480, 50 + CaptureClock::kIdleMs + 1);
        require(clock.count() == 1 && clock.gap(0).frames == -1, "a device that was idle claims to continue");
        for (int index = 0; index < 20; ++index) clock.packet(480, 480, CaptureClock::kDiscontinuity, 1000000 + 1000ull * index, 560 + index);
        require(clock.count() == CaptureClock::kMaxGaps && clock.gap(CaptureClock::kMaxGaps - 1).frames == -1, "losses beyond the list are forgotten");
    }
    bool rejected = false;
    try { WindReferenceSet::fromSamples({Samples(1000)}); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "a recording shorter than two seconds is accepted");
}

void testScenarios(const std::string& label, const std::vector<Samples>& raw, const std::shared_ptr<const WindReferenceSet>& references, bool thorough) {
    struct Case { const char* name; Scenario scenario; };
    std::vector<Case> cases;
    const auto add = [&](const char* name, auto edit) { Scenario s; edit(s); cases.push_back({name, s}); };
    add("-6 dB", [](Scenario&) {});
    add("-20 dB", [](Scenario& s) { s.gainDb = -20; s.footsteps = 0.004; });
    add("second recording", [&](Scenario& s) { s.reference = raw.size() - 1; s.position = 123456.2; });
    add("drift and muffling", [](Scenario& s) { s.ppm = 100; s.cutoff = 3000; s.eqDb = 1; s.gainDb = -10; });
    add("busy mix 44.1 kHz", [](Scenario& s) { s.rate = 44100; s.ppm = -100; s.voice = 0.015; s.shots = 0.1; s.gainDb = -8; });
    add("busy mix 96 kHz", [](Scenario& s) { s.rate = 96000; s.voice = 0.015; s.shots = 0.1; s.cutoff = 6000; });
    if (thorough) {
        add("heavily muffled", [](Scenario& s) { s.cutoff = 800; s.gainDb = -8; s.ppm = -60; });
        add("loud shots", [](Scenario& s) { s.shots = 0.25; s.gainDb = -12; s.seed = 4; });
        add("loop start", [&](Scenario& s) { s.reference = raw.size() - 1; s.position = static_cast<double>(raw.back().size() / 2) - 4.5 * 48000; });
    }
    double worst = -200, slowest = 0, lost = 0;
    for (const auto& item : cases) {
        const auto outcome = play(item.scenario, raw, references);
        const std::string name = label + " " + item.name + ": ";
        std::cout << "Wind " << name << "lock " << outcome.lockSeconds << " s, residual " << outcome.residualDb << " dB, footsteps " << outcome.footstepDb << " dB, worst step " << outcome.worstStepDb << " dB of " << outcome.steps << "\n";
        require(outcome.lockSeconds > 0 && outcome.lockSeconds <= 3, name + "not recognised within 3 s");
        require(outcome.reference == static_cast<int>(item.scenario.reference), name + "wrong recording");
        require(outcome.residualDb <= -15, name + "wind is reduced by less than 15 dB");
        require(std::abs(outcome.footstepDb) <= 1, name + "footsteps change by more than 1 dB");
        require(outcome.steps > 5 && std::abs(outcome.worstStepDb) <= 1, name + "one footstep changes by " + std::to_string(outcome.worstStepDb) + " dB");
        require(outcome.losses == 0, name + "lock lost");
        worst = std::max(worst, outcome.residualDb);
        slowest = std::max(slowest, outcome.lockSeconds);
        lost = std::max(lost, std::abs(outcome.footstepDb));
    }
    std::cout << "Wind " << label << " worst residual " << worst << " dB, slowest lock " << slowest << " s, largest footstep change " << lost << " dB\n";
    {
        Scenario s;
        s.stopAt = 5;
        s.seconds = 8;
        const auto outcome = play(s, raw, references);
        require(outcome.lockSeconds > 0 && outcome.residualDb <= -15, label + " wind that stops was not subtracted before");
        require(outcome.silentSeconds > 0 && outcome.silentSeconds <= 2, label + " wind that stops is still subtracted after " + std::to_string(outcome.silentSeconds) + " s");
        require(outcome.worstSample <= outcome.windPeak * 1.2, label + " stopping wind pops");
    }
    for (const bool known : {true, false}) {
        Scenario s;
        s.gapAt = 5;
        s.gapKnown = known;
        const auto outcome = play(s, raw, references);
        require(outcome.recoverSeconds > 0 && outcome.recoverSeconds <= 1, label + (known ? " known" : " unknown") + " gap not recovered: " + std::to_string(outcome.recoverSeconds));
    }
    // A tracker that gets no processor time catches up on the same timeline: short of what the ring holds, far
    // beyond it, and with more lost packets than there are slots for them.
    for (const int kind : {0, 1, 2, 3}) for (const unsigned rate : {48000u, 44100u}) {
        if (kind == 1 && rate != 48000) continue;
        Scenario s;
        s.rate = rate;
        s.stallAt = 4;
        s.stallSeconds = kind == 0 ? 2 : kind == 1 ? 12 : kind == 2 ? 6.5 : 3;
        s.stallGap = kind == 3 ? 5 : 0;
        s.seconds = s.stallAt + s.stallSeconds + 5;
        const auto outcome = play(s, raw, references);
        require(outcome.tailDb <= -15, label + " wind is not subtracted after a stall of " + std::to_string(s.stallSeconds) + " s: " + std::to_string(outcome.tailDb) + " dB");
    }
}

// A lock found before a break of unknown length is out of place behind it, also when the audio side has not read it yet.
void testStaleLock(const std::vector<Samples>& raw, const std::shared_ptr<const WindReferenceSet>& references) {
    WindStage stage(48000, 2, references);
    constexpr std::size_t block = 480;
    double position = 500000;
    std::vector<float> buffer(block * 2);
    const auto fill = [&] {
        for (std::size_t frame = 0; frame < block; ++frame, position += 1) for (std::size_t channel = 0; channel < 2; ++channel)
            buffer[frame * 2 + channel] = static_cast<float>(at(raw[0], position, channel) * 0.5);
    };
    for (int blocks = 0; blocks < 400 && !stage.tracker()->lock().locked; ++blocks) {
        fill();
        stage.process(buffer.data(), block);
        stage.pump();
    }
    require(stage.tracker()->lock().locked && stage.silent(), "the lock is not waiting to be read");
    position += 5000;
    stage.gap(-1);
    for (int blocks = 0; blocks < 30; ++blocks) {
        fill();
        const auto before = buffer;
        stage.process(buffer.data(), block);
        require(buffer == before, "a lock from before a break is used behind it");
    }
    for (int blocks = 0; blocks < 400; ++blocks) { fill(); stage.process(buffer.data(), block); stage.pump(); }
    require(stage.status().state == WindState::locked && stage.canceller().appliedGain() > 0.3, "the wind is not found again behind a break");
}

// Whatever was lost right before counts for nothing once nobody knows how much time went by.
void testReturnAfterIdle(const std::vector<Samples>& raw, const std::shared_ptr<const WindReferenceSet>& references) {
    WindStage stage(48000, 2, references);
    constexpr std::size_t block = 480;
    std::vector<float> buffer(block * 2);
    const auto play = [&](double position, double seconds, double level) {
        double locked = -1;
        for (std::size_t blocks = 0; blocks < static_cast<std::size_t>(seconds * 100); ++blocks) {
            for (std::size_t frame = 0; frame < block; ++frame) for (std::size_t channel = 0; channel < 2; ++channel)
                buffer[frame * 2 + channel] = static_cast<float>(level > 0 ? at(raw[0], position + static_cast<double>(blocks * block + frame), channel) * level : 0);
            stage.process(buffer.data(), block);
            stage.pump();
            if (locked < 0 && stage.status().state == WindState::locked) locked = static_cast<double>(blocks + 1) / 100;
        }
        return locked;
    };
    require(play(500000, 4, 0.5) > 0, "the wind is not found");
    play(0, 1.5, 0);
    require(stage.status().state == WindState::searching, "silence keeps the lock");
    stage.gap(-1);
    const double again = play(200000, 4, 0.5);
    require(again > 0 && again <= 3, "wind that starts over after an idle stream takes " + std::to_string(again) + " s");
    // Half a minute of sound without wind is told from a search that has only begun; silence does not count.
    play(0, 1.5, 0);
    Noise noise(9);
    const auto listen = [&](double seconds, double level) {
        for (std::size_t blocks = 0; blocks < static_cast<std::size_t>(seconds * 100); ++blocks) {
            for (auto& sample : buffer) sample = static_cast<float>(noise.gauss() * level);
            stage.process(buffer.data(), block);
            stage.pump();
        }
        return stage.status().state;
    };
    require(listen(40, 0) == WindState::searching, "silence is taken for a search without result");
    require(listen(25, 0.01) == WindState::searching, "a search is given up early");
    require(listen(10, 0.01) == WindState::unrecognized, "a search without result is not told");
    require(play(300000, 4, 0.5) > 0 && stage.status().state == WindState::locked, "the wind is not found after a long search");
}

void testNoFalseLock(const std::shared_ptr<const WindReferenceSet>& references) {
    for (int kind = 0; kind < 4; ++kind) {
        Noise noise(static_cast<unsigned long long>(kind) + 40);
        const std::size_t frames = 48000 * 20;
        std::vector<float> input(frames * 2);
        Biquad low[2] = {Biquad::lowpass(600, 48000), Biquad::lowpass(600, 48000)};
        double phase[6] = {}, pitch[6] = {}, envelope = 0;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const double t = static_cast<double>(frame) / 48000;
            if (frame % 12000 == 0) { for (auto& value : pitch) value = 110 * std::pow(2.0, std::floor(noise.uniform() * 24) / 12); envelope = 1; }
            envelope *= 0.99995;
            double chord = 0;
            for (int voice = 0; voice < 6; ++voice) { phase[voice] += 2 * kPi * pitch[voice] / 48000; chord += std::sin(phase[voice]) / 6; }
            const double a = noise.gauss(), b = noise.gauss(), c = noise.gauss();
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const double own = channel ? b : c;
                double value = 0;
                if (kind == 0) value = 0.1 * chord * envelope + 0.0005 * own;
                if (kind == 1) value = 0.05 * low[channel].run(0.7 * a + 0.7 * own);
                if (kind == 2) value = 0.05 * low[channel].run(a) * (std::sin(2 * kPi * 4 * t) > 0 ? 1 : 0.05) + 0.001 * own;
                if (kind == 3) value = 0.08 * chord * envelope + 0.03 * low[channel].run(0.7 * a + 0.7 * own) * (0.5 + 0.5 * std::sin(2 * kPi * 0.3 * t));
                input[frame * 2 + channel] = static_cast<float>(value);
            }
        }
        auto output = input;
        WindStage stage(48000, 2, references);
        for (std::size_t frame = 0; frame < frames; frame += 480) {
            stage.process(output.data() + frame * 2, 480);
            stage.pump();
            require(stage.status().state == WindState::searching, "sound without wind locks");
        }
        require(stage.tracker()->searches() > 30, "sound without wind is not even searched");
        require(output == input, "sound without wind is changed");
    }
}

void testChannels(const std::vector<Samples>& raw, const std::shared_ptr<const WindReferenceSet>& references) {
    constexpr unsigned channels = 8;
    WindStage stage(48000, channels, references);
    const std::size_t frames = 48000 * 4;
    std::vector<float> input(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) for (unsigned channel = 0; channel < channels; ++channel)
        input[frame * channels + channel] = static_cast<float>(at(raw[0], 300000.0 + static_cast<double>(frame), channel % 2) * 0.5);
    auto output = input;
    for (std::size_t frame = 0; frame < frames; frame += 480) { stage.process(output.data() + frame * channels, 480); stage.pump(); }
    double before = 0, after = 0;
    for (std::size_t frame = 48000 * 3; frame < frames; ++frame) {
        for (unsigned channel = 2; channel < channels; ++channel) require(output[frame * channels + channel] == input[frame * channels + channel], "a channel beyond the first two is changed");
        for (unsigned channel = 0; channel < 2; ++channel) {
            before += std::pow(static_cast<double>(input[frame * channels + channel]), 2);
            after += std::pow(static_cast<double>(output[frame * channels + channel]), 2);
        }
    }
    require(10 * std::log10(after / before) < -30, "the first two of eight channels are not cleaned");
    // Switching off fades out and leaves the rest untouched; silence stays silent.
    stage.setActive(false);
    std::vector<float> tail(480 * 20 * channels);
    for (std::size_t frame = 0; frame < 480 * 20; ++frame) for (unsigned channel = 0; channel < channels; ++channel)
        tail[frame * channels + channel] = static_cast<float>(at(raw[0], 300000.0 + static_cast<double>(frames + frame), channel % 2) * 0.5);
    const auto original = tail;
    for (std::size_t frame = 0; frame < 480 * 20; frame += 480) { stage.process(tail.data() + frame * channels, 480); stage.pump(); }
    require(stage.silent(), "a stage switched off keeps subtracting");
    for (std::size_t index = 480 * 10 * channels; index < tail.size(); ++index) require(tail[index] == original[index], "a stage switched off still changes audio");
}
}

void runWindTests() {
    testBasics();
    testLoading();
    const std::vector<Samples> synthetic = {syntheticWind(1, 48000 * 40), syntheticWind(2, 48000 * 30)};
    const auto syntheticSet = WindReferenceSet::fromSamples(synthetic);
    testScenarios("synthetic", synthetic, syntheticSet, false);
    testNoFalseLock(syntheticSet);
    testChannels(synthetic, syntheticSet);
    testStaleLock(synthetic, syntheticSet);
    testReturnAfterIdle(synthetic, syntheticSet);

    const auto file = std::filesystem::path(executablePath()).parent_path() / WindReferenceSet::kFileName;
    if (!std::filesystem::exists(file)) {
        std::cout << "Wind recordings absent: their own scenarios are skipped\n";
        return;
    }
    // A stage waits for a file that another program holds for a moment.
    {
        const HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(held != INVALID_HANDLE_VALUE, "the test could not hold the recordings");
        WindStage stage(48000, 2, file);
        Sleep(200);
        const bool waiting = stage.status().state == WindState::loading;
        CloseHandle(held);
        bool ready = false;
        for (int attempt = 0; attempt < 600 && !ready; ++attempt) { ready = stage.status().state == WindState::searching; if (!ready) Sleep(10); }
        require(waiting && ready, "a recording file that is held for a moment is given up");
    }
    // Loading re-creates the published float samples bit for bit, so 16-bit storage costs nothing.
    const auto set = WindReferenceSet::load(file);
    require(set != nullptr, "the distributed recordings are not the published ones");
    std::vector<Samples> raw;
    for (const auto& item : set->items()) raw.push_back(item.samples);
    testScenarios("published", raw, set, true);
    testNoFalseLock(set);
}
} // namespace hikari
