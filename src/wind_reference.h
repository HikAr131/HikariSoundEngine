// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "wind_constants.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hikari {
// One looping 48 kHz stereo recording with its fingerprint.
struct WindReference {
    std::size_t frames = 0, hops = 0;
    std::vector<std::int16_t> samples;
    // Band-major; each band holds every hop followed by the kSubFrames - 1 that come after the end of the loop.
    std::vector<float> fingerprint;
    std::array<double, wind::kBands> profile{};
    std::size_t stride() const noexcept { return hops + wind::kSubFrames - 1; }
    const float* band(std::size_t index) const noexcept { return fingerprint.data() + index * stride(); }
    double wrap(double position) const noexcept;
    // Signed circular distance a - b, within half the length.
    double distance(double a, double b) const noexcept;
    void linear(double position, std::size_t count, float* stereo) const noexcept;
};

// Kaiser-windowed sinc interpolation of a reference at fractional positions.
class WindInterpolator {
public:
    WindInterpolator();
    void read(const WindReference& reference, double position, float& left, float& right) const noexcept;
    const float* phase(std::size_t index) const noexcept { return table_.data() + index * wind::kSincTaps; }
private:
    std::vector<float> table_;
};

class WindReferenceSet {
public:
    static constexpr std::size_t kMaxSegments = 5;
    static constexpr std::size_t kMinFrames = 96000;
    static constexpr std::uintmax_t kMaxFileBytes = 64u * 1024u * 1024u;
    static constexpr char kFileName[] = "delta-wind-refs.bin";
    // SHA-256 of the float32 form of every published segment, in file order.
    static const std::vector<std::string>& publishedDigests();
    // Null when the file is absent, malformed, or not exactly the expected recordings. `unreadable` tells a file
    // that is there but could not be read from one with the wrong content; `stop` gives up early.
    static std::shared_ptr<const WindReferenceSet> load(const std::filesystem::path& file,
        const std::vector<std::string>& digests = publishedDigests(), bool* unreadable = nullptr, const std::atomic<bool>* stop = nullptr);
    static std::shared_ptr<const WindReferenceSet> fromSamples(std::vector<std::vector<std::int16_t>> segments, const std::atomic<bool>* stop = nullptr);
    const std::vector<WindReference>& items() const noexcept { return items_; }
    const WindInterpolator& interpolator() const noexcept { return interpolator_; }
private:
    std::vector<WindReference> items_;
    WindInterpolator interpolator_;
};
} // namespace hikari
