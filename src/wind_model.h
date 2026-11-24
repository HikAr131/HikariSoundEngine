// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "fft.h"
#include "wind_constants.h"
#include <array>
#include <complex>
#include <cstdint>
#include <optional>
#include <vector>

namespace hikari {
// Second-order Butterworth low-pass at 48 kHz; cutoffs at the top of the range pass everything.
struct WindLowPass {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    static WindLowPass design(double cutoff, double rate = wind::kRate) noexcept;
};

// Compares the input with the aligned reference: how loud and how muffled the game plays it, whether it is
// playing at all, how well the model explains the input, and by how much the alignment is off. Spectra are
// averaged with the loudest moments turned down, so that shots and voices disturb the comparison less.
class WindModel {
public:
    struct Delay { double offset = 0, variance = 0; };
    struct BandQuality { double value = 1, error = 0; bool reliable = false; };
    WindModel();
    void reset() noexcept;
    // Keeps the fitted model, forgets the running spectra; used after the alignment moved.
    void resetStatistics() noexcept;
    void feed(const float* input, const float* aligned, std::size_t frames);
    // First fit of a new lock over everything fed since the reset. The muffling also delays the sound, so both are
    // found together; returns by how many frames the alignment has to move.
    std::optional<double> begin();
    // Call every kTrackEveryBlocks blocks once fitted.
    void estimate();
    bool fitted() const noexcept { return fitted_; }
    bool ready() const noexcept { return frames_ >= wind::kSpectrumFrames; }
    double gain() const noexcept { return fitted_ ? gain_ : 0; }
    double cutoff() const noexcept { return cutoff_; }
    double presence() const noexcept { return presence_; }
    double quality() const;
    std::array<BandQuality, 4> bandQualities() const;
    // How far behind the true position a plain correlation would place the muffled reference.
    double apparentDelay() const;
    // Offset of the true position against the aligned one, in frames; empty without enough evidence.
    std::optional<Delay> spectralDelay() const;
    std::optional<double> highBandDelay();

private:
    static constexpr std::size_t kBins = wind::kSpectrumFft / 2 + 1;
    static constexpr std::size_t kGroups = 14;
    static constexpr int kLagSpan = wind::kTrackRadius + static_cast<int>(wind::kSincTaps) / 2 + 1;
    static constexpr std::size_t kLags = 2 * kLagSpan + 1;
    static constexpr std::size_t kHistory = wind::kTrackFrames + 2 * kLagSpan;
    using Complex = std::complex<double>;
    using Response = std::array<Complex, kBins>;
    struct Frame {
        std::array<float, kBins> xx{}, yy{};
        std::array<std::complex<float>, kBins> xy{};
        std::array<float, kGroups> weight{};
    };
    struct Spectra {
        std::array<double, kBins> xx{}, yy{};
        Response xy{};
        std::array<double, kGroups> weight{}, square{};
        void add(const Frame& frame, double sign, double decay = 1) noexcept;
    };
    struct TrackFilter {
        double z1[4][2] = {}, z2[4][2] = {};
        double process(std::size_t channel, double value) noexcept;
    };
    struct Evidence {
        std::size_t count = 0;
        std::array<std::size_t, kBins> bin;
        std::array<double, kBins> omega, phase, weight;
    };
    struct Fit { double gain = 0, cutoff = 0, error = 0; };

    void spectrum();
    void fit();
    std::optional<Fit> solve(const Spectra& spectra) const;
    double cutoffLimit(const Spectra& spectra, double gain) const;
    void gather(const Spectra& spectra, Evidence& evidence) const;
    static double delayOf(const Evidence& evidence, const Complex* response, double low, double high, double& agreement);
    void updatePresence();
    void responses(double cutoff, Response& output) const noexcept;
    // Independent time-frequency cells behind one bin.
    double cells(const Spectra& spectra, std::size_t bin) const noexcept;

    Fft<float> fft_{wind::kSpectrumFft};
    std::array<float, wind::kSpectrumFft> hann_{};
    Response z1_{}, z2_{};
    std::vector<std::array<std::complex<float>, kBins>> candidates_;
    std::array<double, wind::kCutoffs> cutoffs_{};
    std::array<std::size_t, kBins> group_{};
    std::array<double, kGroups> typical_{};
    bool typicalKnown_ = false;
    std::vector<Frame> history_;
    Frame incoming_;
    Spectra short_, long_;
    std::size_t frames_ = 0, next_ = 0;
    std::vector<float> input_, aligned_;
    std::size_t filled_ = 0;
    TrackFilter inputFilter_, alignedFilter_;
    std::array<std::vector<float>, 2> highInput_, highAligned_;
    std::size_t highPosition_ = 0, highFilled_ = 0;
    bool fitted_ = false;
    double gain_ = 0, cutoff_ = wind::kNoCutoffHz, presence_ = 1;
    std::array<double, wind::kPresenceRing> ring_{};
    std::size_t ringFilled_ = 0, ringPosition_ = 0;
};
} // namespace hikari
