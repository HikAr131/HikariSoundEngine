// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "wind_reference.h"
#include "sha256.h"
#include "wind_fingerprint.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace hikari {
using namespace wind;
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr float kSampleScale = 1.0f / 32768.0f;
constexpr std::uint32_t kFormatVersion = 1;

double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 96; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

std::uint64_t little(const unsigned char* bytes, int count) noexcept {
    std::uint64_t value = 0;
    for (int index = count - 1; index >= 0; --index) value = value << 8 | bytes[index];
    return value;
}

bool stopped(const std::atomic<bool>* stop) noexcept { return stop && stop->load(std::memory_order_relaxed); }

bool buildFingerprint(WindReference& reference, const std::atomic<bool>* stop) {
    const WindFingerprint fingerprint;
    reference.hops = reference.frames / kSubFrame;
    reference.fingerprint.assign(kBands * reference.stride(), 0.0f);
    std::array<double, kBands> mean{}, power{};
    std::vector<float> frame(kSubFrame * 2);
    // The hops behind the end go on across the joint of the loop, which need not fall between two sub-frames.
    for (std::size_t hop = 0; hop < reference.stride(); ++hop) {
        if (stopped(stop)) return false;
        for (std::size_t index = 0; index < kSubFrame; ++index) {
            const std::size_t at = (hop * kSubFrame + index) % reference.frames;
            for (std::size_t channel = 0; channel < 2; ++channel) frame[index * 2 + channel] = static_cast<float>(reference.samples[at * 2 + channel]) * kSampleScale;
        }
        const auto energies = fingerprint.analyze(frame.data());
        for (std::size_t band = 0; band < kBands; ++band) {
            reference.fingerprint[band * reference.stride() + hop] = energies[band];
            if (hop >= reference.hops) continue;
            mean[band] += energies[band];
            power[band] += std::exp(static_cast<double>(energies[band]));
        }
    }
    for (std::size_t band = 0; band < kBands; ++band) {
        auto* values = reference.fingerprint.data() + band * reference.stride();
        const double average = mean[band] / static_cast<double>(reference.hops);
        for (std::size_t hop = 0; hop < reference.stride(); ++hop) values[hop] = static_cast<float>(values[hop] - average);
        power[band] /= static_cast<double>(reference.hops);
    }
    reference.profile = windProfile(power);
    return true;
}

std::string floatDigest(const std::vector<std::int16_t>& samples) {
    Sha256 hash;
    std::vector<float> chunk;
    constexpr std::size_t size = 1 << 16;
    for (std::size_t start = 0; start < samples.size(); start += size) {
        const std::size_t count = std::min(size, samples.size() - start);
        chunk.resize(count);
        for (std::size_t index = 0; index < count; ++index) chunk[index] = static_cast<float>(samples[start + index]) * kSampleScale;
        hash.update(chunk.data(), count * sizeof(float));
    }
    return Sha256::hex(hash.finish());
}
}

double WindReference::wrap(double position) const noexcept {
    const double length = static_cast<double>(frames);
    const double wrapped = std::fmod(position, length);
    return wrapped < 0 ? wrapped + length : wrapped;
}

double WindReference::distance(double a, double b) const noexcept {
    const double length = static_cast<double>(frames);
    const double difference = std::fmod(a - b, length);
    if (difference > length / 2) return difference - length;
    return difference < -length / 2 ? difference + length : difference;
}

void WindReference::linear(double position, std::size_t count, float* stereo) const noexcept {
    const double start = wrap(position);
    auto index = static_cast<std::size_t>(start);
    const auto fraction = static_cast<float>(start - static_cast<double>(index));
    if (index >= frames) index = 0;
    for (std::size_t frame = 0; frame < count; ++frame) {
        const std::size_t next = index + 1 == frames ? 0 : index + 1;
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto a = static_cast<float>(samples[index * 2 + channel]), b = static_cast<float>(samples[next * 2 + channel]);
            stereo[frame * 2 + channel] = (a + (b - a) * fraction) * kSampleScale;
        }
        index = next;
    }
}

WindInterpolator::WindInterpolator() : table_((kSincPhases + 1) * kSincTaps) {
    const double scale = besselI0(kSincBeta), half = static_cast<double>(kSincTaps) / 2;
    for (std::size_t phase = 0; phase <= kSincPhases; ++phase) {
        const double fraction = static_cast<double>(phase) / static_cast<double>(kSincPhases);
        std::array<double, kSincTaps> taps{};
        double sum = 0;
        for (std::size_t tap = 0; tap < kSincTaps; ++tap) {
            const double offset = static_cast<double>(tap) - (half - 1) - fraction;
            const double sinc = std::abs(offset) < 1e-12 ? 1 : std::sin(kPi * offset) / (kPi * offset);
            taps[tap] = sinc * besselI0(kSincBeta * std::sqrt(std::max(0.0, 1 - (offset / half) * (offset / half)))) / scale;
            sum += taps[tap];
        }
        for (std::size_t tap = 0; tap < kSincTaps; ++tap) table_[phase * kSincTaps + tap] = static_cast<float>(taps[tap] / sum);
    }
}

void WindInterpolator::read(const WindReference& reference, double position, float& left, float& right) const noexcept {
    const double wrapped = reference.wrap(position);
    const double whole = std::floor(wrapped);
    const auto phase = static_cast<std::size_t>((wrapped - whole) * static_cast<double>(kSincPhases) + 0.5);
    const float* taps = table_.data() + phase * kSincTaps;
    const auto center = static_cast<std::int64_t>(whole);
    const auto length = static_cast<std::int64_t>(reference.frames);
    std::int64_t first = center - static_cast<std::int64_t>(kSincTaps) / 2 + 1;
    float sumLeft = 0, sumRight = 0;
    if (first >= 0 && first + static_cast<std::int64_t>(kSincTaps) <= length) {
        const auto* source = reference.samples.data() + first * 2;
        for (std::size_t tap = 0; tap < kSincTaps; ++tap) {
            sumLeft += taps[tap] * static_cast<float>(source[tap * 2]);
            sumRight += taps[tap] * static_cast<float>(source[tap * 2 + 1]);
        }
    } else {
        for (std::size_t tap = 0; tap < kSincTaps; ++tap, ++first) {
            std::int64_t index = first % length;
            if (index < 0) index += length;
            sumLeft += taps[tap] * static_cast<float>(reference.samples[static_cast<std::size_t>(index) * 2]);
            sumRight += taps[tap] * static_cast<float>(reference.samples[static_cast<std::size_t>(index) * 2 + 1]);
        }
    }
    left = sumLeft * kSampleScale;
    right = sumRight * kSampleScale;
}

const std::vector<std::string>& WindReferenceSet::publishedDigests() {
    static const std::vector<std::string> digests = {
        "e2bc9340a6a09adbcbc127e87f1613de42e6e01fd07ccdae89da60ed798667d1",
        "eeca18e147ffbeb90a5a2ac96e639f7b112d83319e8c7fa0dfd9904f9e3d2425",
    };
    return digests;
}

std::shared_ptr<const WindReferenceSet> WindReferenceSet::fromSamples(std::vector<std::vector<std::int16_t>> segments, const std::atomic<bool>* stop) {
    if (segments.empty() || segments.size() > kMaxSegments) throw std::invalid_argument("Unsupported reference count");
    auto set = std::make_shared<WindReferenceSet>();
    for (auto& samples : segments) {
        if (samples.size() % 2 != 0 || samples.size() / 2 < kMinFrames) throw std::invalid_argument("Reference is too short");
        WindReference reference;
        reference.frames = samples.size() / 2;
        reference.samples = std::move(samples);
        if (!buildFingerprint(reference, stop)) return nullptr;
        set->items_.push_back(std::move(reference));
    }
    return set;
}

std::shared_ptr<const WindReferenceSet> WindReferenceSet::load(const std::filesystem::path& file, const std::vector<std::string>& digests,
    bool* unreadable, const std::atomic<bool>* stop) {
    const auto unread = [unreadable]() -> std::shared_ptr<const WindReferenceSet> {
        if (unreadable) *unreadable = true;
        return nullptr;
    };
    if (unreadable) *unreadable = false;
    try {
        std::error_code code;
        const auto bytes = std::filesystem::file_size(file, code);
        if (code && code != std::errc::no_such_file_or_directory) return unread();
        if (code || bytes < 12 || bytes > kMaxFileBytes) return nullptr;
        std::ifstream input(file, std::ios::binary);
        unsigned char header[12];
        if (!input.read(reinterpret_cast<char*>(header), sizeof(header))) return unread();
        if (std::memcmp(header, "HKWR", 4) != 0 || little(header + 4, 4) != kFormatVersion) return nullptr;
        const auto count = static_cast<std::size_t>(little(header + 8, 4));
        if (count == 0 || count > kMaxSegments || count != digests.size()) return nullptr;
        constexpr std::size_t entryBytes = 4 + 4 + 8 + 32;
        std::vector<std::size_t> frames;
        std::uintmax_t expected = sizeof(header) + count * entryBytes;
        if (bytes < expected) return nullptr;
        for (std::size_t index = 0; index < count; ++index) {
            unsigned char entry[entryBytes];
            if (!input.read(reinterpret_cast<char*>(entry), sizeof(entry))) return unread();
            const auto length = little(entry + 8, 8);
            if (little(entry, 4) != static_cast<std::uint64_t>(kRate) || little(entry + 4, 4) != 2 ||
                length < kMinFrames || length > kMaxFileBytes / 4) return nullptr;
            std::array<std::uint8_t, 32> digest{};
            std::copy_n(entry + 16, digest.size(), digest.begin());
            if (Sha256::hex(digest) != digests[index]) return nullptr;
            frames.push_back(static_cast<std::size_t>(length));
            expected += length * 4;
        }
        if (expected != bytes) return nullptr;
        std::vector<std::vector<std::int16_t>> segments;
        for (std::size_t index = 0; index < count; ++index) {
            std::vector<std::int16_t> samples(frames[index] * 2);
            if (!input.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(std::int16_t)))) return unread();
            if (stopped(stop) || floatDigest(samples) != digests[index]) return nullptr;
            segments.push_back(std::move(samples));
        }
        return fromSamples(std::move(segments), stop);
    } catch (...) {
        return nullptr;
    }
}
} // namespace hikari
