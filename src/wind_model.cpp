// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "wind_model.h"
#include "wind_reference.h"
#include <algorithm>
#include <cmath>

namespace hikari {
using namespace wind;
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kSqrt2 = 1.41421356237309504880;
// Each hop adds a quarter of a transform, and a Hann window smears one and a half bins.
constexpr double kCellsPerFrame = static_cast<double>(kSpectrumHop) / static_cast<double>(kSpectrumFft) / 1.5;
constexpr std::array<double, 15> kGroupEdges = {0, 120, 220, 340, 500, 700, 1000, 1500, 2200, 3250, 4600, 6500, 10000, 16000, 24001};
constexpr double kTypicalStep = 0.02;
constexpr double kMinShare = 0.25;
constexpr double kLongDecay = 0.99;
constexpr double kDelayLowHz = 100, kDelayHighHz = 9000;
constexpr double kDelayMinInformation = 0.4;
constexpr double kDelayMinAgreement = 0.6;
constexpr double kMinEvidence = 6;
// A 500 Hz low-pass delays by about 22 frames.
constexpr double kMaxMuffleDelay = 30;
constexpr double kBeginMaxError = 0.2;
// The modelled wind cannot be louder than everything that is heard, give or take the averaging.
constexpr double kLouderThanInput = 2;

std::size_t lowBin(double hz) { return static_cast<std::size_t>(std::ceil(hz * static_cast<double>(kSpectrumFft) / kRate)); }
}

WindLowPass WindLowPass::design(double cutoff, double rate) noexcept {
    WindLowPass filter;
    if (cutoff >= kBypassCutoffHz || cutoff >= rate / 2 * 0.9995) return filter;
    const double omega = 2 * kPi * std::max(cutoff, 10.0) / rate;
    const double c = std::cos(omega), alpha = std::sin(omega) / kSqrt2, a0 = 1 + alpha;
    filter.b0 = filter.b2 = (1 - c) / (2 * a0);
    filter.b1 = (1 - c) / a0;
    filter.a1 = -2 * c / a0;
    filter.a2 = (1 - alpha) / a0;
    return filter;
}

double WindModel::TrackFilter::process(std::size_t channel, double value) noexcept {
    for (std::size_t section = 0; section < kTrackFilter.size(); ++section) {
        const auto& c = kTrackFilter[section];
        const double out = c.b0 * value + z1[section][channel];
        z1[section][channel] = c.b1 * value - c.a1 * out + z2[section][channel];
        z2[section][channel] = c.b2 * value - c.a2 * out;
        value = out;
    }
    return value;
}

void WindModel::Spectra::add(const Frame& frame, double sign, double decay) noexcept {
    for (std::size_t bin = 0; bin < kBins; ++bin) {
        xx[bin] = std::max(0.0, xx[bin] * decay + sign * frame.xx[bin]);
        yy[bin] = std::max(0.0, yy[bin] * decay + sign * frame.yy[bin]);
        xy[bin] = xy[bin] * decay + sign * Complex(frame.xy[bin]);
    }
    for (std::size_t group = 0; group < kGroups; ++group) {
        const double value = frame.weight[group];
        weight[group] = weight[group] * decay + sign * value;
        square[group] = square[group] * decay * decay + sign * value * value;
    }
}

WindModel::WindModel() : candidates_(kCutoffs), history_(kSpectrumFrames),
    input_(2 * kSpectrumFft), aligned_(2 * kSpectrumFft) {
    for (std::size_t n = 0; n < kSpectrumFft; ++n)
        hann_[n] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(n) / static_cast<double>(kSpectrumFft - 1)));
    for (std::size_t bin = 0; bin < kBins; ++bin) {
        const double omega = 2 * kPi * static_cast<double>(bin) / static_cast<double>(kSpectrumFft);
        z1_[bin] = std::polar(1.0, -omega);
        z2_[bin] = std::polar(1.0, -2 * omega);
        const double hz = static_cast<double>(bin) * kRate / static_cast<double>(kSpectrumFft);
        while (group_[bin] + 1 < kGroups && hz >= kGroupEdges[group_[bin] + 1]) ++group_[bin];
    }
    for (std::size_t index = 0; index < kCutoffs; ++index) {
        cutoffs_[index] = index + 1 == kCutoffs ? kNoCutoffHz
            : kCutoffLowHz * std::pow(kCutoffSpan, static_cast<double>(index) / static_cast<double>(kCutoffs - 2));
        Response h;
        responses(cutoffs_[index], h);
        for (std::size_t bin = 0; bin < kBins; ++bin) candidates_[index][bin] = std::complex<float>(h[bin]);
    }
    for (auto& channel : highInput_) channel.resize(kHistory);
    for (auto& channel : highAligned_) channel.resize(kHistory);
    reset();
}

void WindModel::responses(double cutoff, Response& output) const noexcept {
    const auto filter = WindLowPass::design(cutoff);
    for (std::size_t bin = 0; bin < kBins; ++bin)
        output[bin] = (filter.b0 + filter.b1 * z1_[bin] + filter.b2 * z2_[bin]) / (1.0 + filter.a1 * z1_[bin] + filter.a2 * z2_[bin]);
}

double WindModel::cells(const Spectra& spectra, std::size_t bin) const noexcept {
    const auto group = group_[bin];
    if (!(spectra.square[group] > 0)) return 0;
    return kCellsPerFrame * spectra.weight[group] * spectra.weight[group] / spectra.square[group];
}

void WindModel::reset() noexcept {
    resetStatistics();
    typicalKnown_ = false;
    fitted_ = false;
    gain_ = 0;
    cutoff_ = kNoCutoffHz;
    presence_ = 1;
    ringFilled_ = ringPosition_ = 0;
}

void WindModel::resetStatistics() noexcept {
    short_ = Spectra();
    long_ = Spectra();
    frames_ = next_ = 0;
    filled_ = 0;
    inputFilter_ = {};
    alignedFilter_ = {};
    highPosition_ = highFilled_ = 0;
}

void WindModel::feed(const float* input, const float* aligned, std::size_t frames) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            highInput_[channel][highPosition_] = static_cast<float>(inputFilter_.process(channel, input[frame * 2 + channel]));
            highAligned_[channel][highPosition_] = static_cast<float>(alignedFilter_.process(channel, aligned[frame * 2 + channel]));
        }
        highPosition_ = highPosition_ + 1 == kHistory ? 0 : highPosition_ + 1;
        if (highFilled_ < kHistory) ++highFilled_;
        if (filled_ == kSpectrumFft) {
            std::copy(input_.begin() + 2 * kSpectrumHop, input_.end(), input_.begin());
            std::copy(aligned_.begin() + 2 * kSpectrumHop, aligned_.end(), aligned_.begin());
            filled_ -= kSpectrumHop;
        }
        for (std::size_t channel = 0; channel < 2; ++channel) {
            input_[filled_ * 2 + channel] = input[frame * 2 + channel];
            aligned_[filled_ * 2 + channel] = aligned[frame * 2 + channel];
        }
        if (++filled_ == kSpectrumFft) spectrum();
    }
}

void WindModel::spectrum() {
    std::array<std::complex<float>, kSpectrumFft> x, r;
    for (std::size_t n = 0; n < kSpectrumFft; ++n) {
        x[n] = {input_[n * 2] * hann_[n], input_[n * 2 + 1] * hann_[n]};
        r[n] = {aligned_[n * 2] * hann_[n], aligned_[n * 2 + 1] * hann_[n]};
    }
    fft_.transform(x.data());
    fft_.transform(r.data());
    std::array<double, kGroups> power{};
    for (std::size_t bin = 0; bin < kBins; ++bin) {
        const std::size_t mirror = (kSpectrumFft - bin) % kSpectrumFft;
        const auto split = [](std::complex<float> a, std::complex<float> b, std::complex<float>& left, std::complex<float>& right) {
            left = {0.5f * (a.real() + b.real()), 0.5f * (a.imag() - b.imag())};
            right = {0.5f * (a.imag() + b.imag()), 0.5f * (b.real() - a.real())};
        };
        std::complex<float> xl, xr, rl, rr;
        split(x[bin], x[mirror], xl, xr);
        split(r[bin], r[mirror], rl, rr);
        incoming_.xx[bin] = std::norm(xl) + std::norm(xr);
        incoming_.yy[bin] = std::norm(rl) + std::norm(rr);
        incoming_.xy[bin] = xl * std::conj(rl) + xr * std::conj(rr);
        power[group_[bin]] += incoming_.xx[bin];
    }
    // A moment far louder than usual says little about the wind under it.
    std::array<float, kGroups> scale{};
    for (std::size_t group = 0; group < kGroups; ++group) {
        const double level = std::log(power[group] + 1e-24);
        if (!typicalKnown_) typical_[group] = level;
        else typical_[group] += kTypicalStep * std::clamp(level - typical_[group], -3.0, 1.0);
        const double usual = std::exp(typical_[group]);
        incoming_.weight[group] = scale[group] = static_cast<float>(usual / std::max(power[group], kMinShare * usual));
    }
    typicalKnown_ = true;
    for (std::size_t bin = 0; bin < kBins; ++bin) {
        const float weight = scale[group_[bin]];
        incoming_.xx[bin] *= weight;
        incoming_.yy[bin] *= weight;
        incoming_.xy[bin] *= weight;
    }
    auto& slot = history_[next_];
    if (frames_ == kSpectrumFrames) short_.add(slot, -1);
    else ++frames_;
    slot = incoming_;
    short_.add(slot, 1);
    long_.add(slot, 1, kLongDecay);
    next_ = (next_ + 1) % kSpectrumFrames;
}

// Band-wise comparison with every cutoff candidate, relative to the response currently in use.
std::optional<WindModel::Fit> WindModel::solve(const Spectra& spectra) const {
    constexpr std::size_t bands = kFitEdges.size() - 1;
    Response h0;
    responses(cutoff_, h0);
    double peak = 0;
    for (const double value : spectra.yy) peak = std::max(peak, value);
    if (!(peak > 0)) return std::nullopt;
    std::array<std::vector<std::size_t>, bands> bins;
    std::array<Complex, bands> x{};
    std::array<double, bands> power{}, v{}, w{};
    std::array<bool, bands> valid{};
    std::size_t count = 0;
    double totalV = 0;
    for (std::size_t band = 0; band < bands; ++band) {
        std::vector<double> coherences;
        for (std::size_t bin = lowBin(kFitEdges[band]); bin < std::min(kBins, lowBin(kFitEdges[band + 1])); ++bin) {
            if (!(spectra.yy[bin] > kFitPowerRatio * peak) || !(spectra.xx[bin] > 0)) continue;
            const double coherence = std::norm(spectra.xy[bin]) / (spectra.xx[bin] * spectra.yy[bin]);
            if (coherence < kFitCoherence) continue;
            bins[band].push_back(bin);
            coherences.push_back(coherence);
            x[band] += std::conj(h0[bin]) * spectra.xy[bin];
            power[band] += std::norm(h0[bin]) * spectra.yy[bin];
        }
        if (bins[band].size() < kFitMinBins || !(power[band] > 0)) continue;
        std::nth_element(coherences.begin(), coherences.begin() + static_cast<std::ptrdiff_t>(coherences.size() / 2), coherences.end());
        const double median = coherences[coherences.size() / 2];
        x[band] /= power[band];
        if (median < kFitBandCoherence || !(std::abs(x[band]) > kFitMinMagnitude)) continue;
        valid[band] = true;
        ++count;
        v[band] = std::min(kFitMaxWeight, median / std::max(kFitWeightFloor, 1 - median));
        const double floor = std::max(kFitWeightFloor, std::abs(x[band]));
        w[band] = v[band] / (floor * floor);
        totalV += v[band];
    }
    if (!count) return std::nullopt;
    const auto evaluate = [&](const auto& ratio) {
        Fit result;
        double product = 0, square = 0;
        std::array<Complex, bands> m{};
        for (std::size_t band = 0; band < bands; ++band) {
            if (!valid[band]) continue;
            m[band] = ratio(band);
            product += w[band] * std::real(m[band] * std::conj(x[band]));
            square += w[band] * std::norm(m[band]);
        }
        result.gain = square > 0 ? std::max(0.0, product / square) : 0;
        for (std::size_t band = 0; band < bands; ++band)
            if (valid[band]) result.error += w[band] * std::norm(result.gain * m[band] - x[band]);
        result.error /= totalV;
        return result;
    };
    const bool low = valid[0] || valid[1] || valid[2], high = valid[4] || valid[5] || valid[6] || valid[7];
    Fit chosen;
    if (count < kFitMinBands || !low || !high) {
        // Too little of the spectrum is clean to judge the muffling: keep it and only follow the level.
        chosen = evaluate([](std::size_t) { return Complex(1, 0); });
        chosen.cutoff = cutoff_;
    } else {
        Fit open;
        for (std::size_t candidate = 0; candidate < kCutoffs; ++candidate) {
            const auto& table = candidates_[candidate];
            Fit next = evaluate([&](std::size_t band) {
                Complex sum;
                for (const auto bin : bins[band]) sum += std::conj(h0[bin]) * Complex(table[bin]) * spectra.yy[bin];
                return sum / power[band];
            });
            next.cutoff = cutoffs_[candidate];
            if (candidate == 0 || next.error < chosen.error) chosen = next;
            if (candidate + 1 == kCutoffs) open = next;
        }
        if (!(chosen.error + kFitNoCutoffMargin < open.error)) chosen = open;
    }
    if (!(chosen.gain > kFitMinGain) || !(chosen.gain < kFitMaxGain) || !(chosen.error <= kFitMaxError)) return std::nullopt;
    return chosen;
}

// The largest cutoff whose model is nowhere louder than the whole input. Bands that no cutoff can satisfy say
// something about the level, not about the muffling, and are left out.
double WindModel::cutoffLimit(const Spectra& spectra, double gain) const {
    double limit = kNoCutoffHz;
    for (std::size_t band = 2; band + 1 < kFitEdges.size(); ++band) {
        const std::size_t first = lowBin(kFitEdges[band]), last = std::min(kBins, lowBin(kFitEdges[band + 1]));
        double heard = 0;
        for (std::size_t bin = first; bin < last; ++bin) heard += spectra.xx[bin];
        const auto fits = [&](std::size_t candidate) {
            double model = 0;
            for (std::size_t bin = first; bin < last; ++bin) model += std::norm(candidates_[candidate][bin]) * spectra.yy[bin];
            return gain * gain * model <= kLouderThanInput * heard;
        };
        if (fits(kCutoffs - 1) || !fits(0)) continue;
        std::size_t low = 0, high = kCutoffs - 1;
        while (high - low > 1) {
            const std::size_t middle = (low + high) / 2;
            (fits(middle) ? low : high) = middle;
        }
        limit = std::min(limit, cutoffs_[low]);
    }
    return limit;
}

void WindModel::estimate() {
    if (!ready() || !fitted_) return;
    fit();
    updatePresence();
}

void WindModel::fit() {
    const auto result = solve(short_);
    if (!result) return;
    const double alpha = 1 - std::exp(-kSmoothing);
    const double decibels = std::clamp(20 * std::log10(result->gain / gain_) * alpha, -kGainDownDb, kGainUpDb);
    gain_ *= std::pow(10.0, decibels / 20);
    const double limit = cutoffLimit(short_, gain_);
    const double target = std::min(result->cutoff, limit);
    if (cutoff_ > limit) cutoff_ = limit;
    if (cutoff_ > kSnapCutoffHz && target >= kNoCutoffHz) { cutoff_ = kNoCutoffHz; return; }
    const double octaves = std::clamp(std::log2(target / cutoff_) * alpha, -kCutoffStepOctaves, kCutoffStepOctaves);
    cutoff_ = std::clamp(cutoff_ * std::exp2(octaves), kCutoffLowHz, kNoCutoffHz);
}

std::optional<double> WindModel::begin() {
    fitted_ = false;
    if (!ready()) return std::nullopt;
    Evidence evidence;
    gather(long_, evidence);
    if (static_cast<double>(evidence.count) < kMinEvidence) return std::nullopt;
    struct Choice { double error = 1e30, gain = 0, cutoff = 0, delay = 0; } best, open;
    const auto consider = [&](std::size_t candidate, double low, double high) {
        Response response;
        for (std::size_t bin = 0; bin < kBins; ++bin) response[bin] = Complex(candidates_[candidate][bin]);
        double agreement = 0;
        const double delay = delayOf(evidence, response.data(), low, high, agreement);
        if (agreement < kDelayMinAgreement) return;
        // One real gain against the delay-compensated transfer function, relative to its size.
        std::array<Complex, kBins> transfer;
        double product = 0, square = 0, error = 0, total = 0;
        for (std::size_t index = 0; index < evidence.count; ++index) {
            const std::size_t bin = evidence.bin[index];
            transfer[index] = long_.xy[bin] / long_.yy[bin] * std::polar(1.0, -evidence.omega[index] * delay);
            const double weight = evidence.weight[index] / std::max(1e-12, std::norm(transfer[index]));
            product += weight * std::real(response[bin] * std::conj(transfer[index]));
            square += weight * std::norm(response[bin]);
        }
        if (!(square > 0) || !(product > 0)) return;
        const double gain = product / square;
        for (std::size_t index = 0; index < evidence.count; ++index) {
            error += evidence.weight[index] * std::norm(gain * response[evidence.bin[index]] - transfer[index]) / std::max(1e-12, std::norm(transfer[index]));
            total += evidence.weight[index];
        }
        const Choice choice{error / total, gain, cutoffs_[candidate], delay};
        if (choice.cutoff > cutoffLimit(long_, gain)) return;
        if (candidate + 1 == kCutoffs) open = choice;
        if (choice.error < best.error) best = choice;
    };
    constexpr std::size_t stride = 4;
    const double low = -static_cast<double>(kTrackRadius);
    for (std::size_t candidate = 0; candidate < kCutoffs; candidate += stride) consider(candidate, low, kMaxMuffleDelay);
    if (best.error > 1e29) return std::nullopt;
    if (best.cutoff < kNoCutoffHz) {
        const auto nearest = static_cast<std::size_t>(std::lround(std::log(best.cutoff / kCutoffLowHz) / std::log(kCutoffSpan) * static_cast<double>(kCutoffs - 2)));
        const double center = best.delay;
        for (std::size_t candidate = nearest > stride ? nearest - stride : 0; candidate <= std::min(nearest + stride, kCutoffs - 2); ++candidate)
            consider(candidate, center - 3, center + 3);
        if (!(best.error + kFitNoCutoffMargin < open.error)) best = open;
    }
    if (!(best.gain > kFitMinGain) || !(best.gain < kFitMaxGain) || !(best.error <= kBeginMaxError)) return std::nullopt;
    fitted_ = true;
    gain_ = best.gain;
    cutoff_ = best.cutoff;
    return best.delay;
}

void WindModel::updatePresence() {
    Response h;
    responses(cutoff_, h);
    double sum = 0, weights = 0;
    for (std::size_t band = 0; band + 1 < kFitEdges.size(); ++band) {
        double model = 0, total = 0, independent = 0;
        Complex cross;
        for (std::size_t bin = lowBin(kFitEdges[band]); bin < std::min(kBins, lowBin(kFitEdges[band + 1])); ++bin) {
            const Complex m = gain_ * h[bin];
            model += std::norm(m) * short_.yy[bin];
            cross += std::conj(m) * short_.xy[bin];
            total += short_.xx[bin];
            independent += 2 * cells(short_, bin);
        }
        if (!(independent > 0) || !(model > 1e-20)) continue;
        const double rest = std::max(0.0, total - std::norm(cross) / model) + 1e-6 * model;
        const double bias = model * rest / independent;
        const double share = (std::norm(cross) - bias) / (model * model);
        const double weight = model * model / bias;
        sum += weight * share;
        weights += weight;
    }
    if (!(weights > 0)) return;
    ring_[ringPosition_] = std::clamp(sum / weights, -1.0, 4.0);
    ringPosition_ = (ringPosition_ + 1) % kPresenceRing;
    if (ringFilled_ < kPresenceRing) ++ringFilled_;
    if (ringFilled_ < kPresenceRing) return;
    double mean = 0, variance = 0;
    for (const double value : ring_) mean += value;
    mean /= static_cast<double>(kPresenceRing);
    for (const double value : ring_) variance += (value - mean) * (value - mean);
    variance /= static_cast<double>(kPresenceRing - 1);
    const double error = std::sqrt(variance / (static_cast<double>(kPresenceRing) * kPresenceEffective));
    if (mean + 2 * error < kPresenceLow) presence_ = std::max(0.0, presence_ - kPresenceDown);
    else if (mean - 2 * error > kPresenceHigh) presence_ = std::min(1.0, presence_ + kPresenceUp);
}

double WindModel::quality() const {
    if (!ready() || !fitted_) return 1;
    Response h;
    responses(cutoff_, h);
    double cross = 0, model = 0;
    for (std::size_t bin = lowBin(kQualityLowHz); bin <= lowBin(kQualityHighHz); ++bin) {
        const Complex m = gain_ * h[bin];
        cross += std::real(std::conj(m) * short_.xy[bin]);
        model += std::norm(m) * short_.yy[bin];
    }
    return model <= kQualityFloor ? 1 : cross / model;
}

std::array<WindModel::BandQuality, 4> WindModel::bandQualities() const {
    std::array<BandQuality, 4> result{};
    if (!ready() || !fitted_) return result;
    Response h;
    responses(cutoff_, h);
    const std::array<double, 5> edges = {kFitEdges.front(), kCrossovers[0], kCrossovers[1], kCrossovers[2], kFitEdges.back()};
    for (std::size_t band = 0; band < result.size(); ++band) {
        double cross = 0, model = 0, total = 0, independent = 0;
        for (std::size_t bin = lowBin(edges[band]); bin < std::min(kBins, lowBin(edges[band + 1])); ++bin) {
            const Complex m = gain_ * h[bin];
            cross += std::real(std::conj(m) * short_.xy[bin]);
            model += std::norm(m) * short_.yy[bin];
            total += short_.xx[bin];
            independent += 2 * cells(short_, bin);
        }
        if (!(independent > 0) || !(model > kQualityFloor)) continue;
        result[band].value = cross / model;
        result[band].error = std::sqrt(std::max(0.0, total - cross * cross / model) / (model * independent));
        result[band].reliable = result[band].error < 0.08;
    }
    return result;
}

double WindModel::apparentDelay() const {
    if (!fitted_ || cutoff_ >= kBypassCutoffHz) return 0;
    Response h;
    responses(cutoff_, h);
    double product = 0, square = 0;
    for (std::size_t bin = lowBin(kQualityLowHz); bin < lowBin(kQualityHighHz); ++bin) {
        const double omega = 2 * kPi * static_cast<double>(bin) / static_cast<double>(kSpectrumFft);
        const double weight = std::norm(h[bin]) * (frames_ ? short_.yy[bin] : 1.0);
        product -= weight * omega * std::arg(h[bin]);
        square += weight * omega * omega;
    }
    return square > 0 ? product / square : 0;
}

// Bins that still carry the reference, with the phase between input and aligned reference.
void WindModel::gather(const Spectra& spectra, Evidence& evidence) const {
    evidence.count = 0;
    for (std::size_t bin = lowBin(kDelayLowHz); bin < lowBin(kDelayHighHz); ++bin) {
        if (!(spectra.xx[bin] > 0) || !(spectra.yy[bin] > 0)) continue;
        const double independent = cells(spectra, bin);
        if (!(independent > 1)) continue;
        const double coherence = std::min(0.98, std::norm(spectra.xy[bin]) / (spectra.xx[bin] * spectra.yy[bin]));
        // Unrelated signals still look coherent by about one over the number of independent cells.
        const double excess = coherence - 1 / independent;
        if (excess <= 0.1) continue;
        auto& index = evidence.count;
        evidence.bin[index] = bin;
        evidence.omega[index] = 2 * kPi * static_cast<double>(bin) / static_cast<double>(kSpectrumFft);
        evidence.phase[index] = std::arg(spectra.xy[bin]);
        evidence.weight[index] = 2 * independent * excess / (1 - excess);
        ++index;
    }
}

// Delay that best lines up the phases once the response's own phase is taken out.
double WindModel::delayOf(const Evidence& evidence, const Complex* response, double low, double high, double& agreement) {
    std::array<double, kBins> phase;
    double total = 0;
    for (std::size_t index = 0; index < evidence.count; ++index) {
        phase[index] = evidence.phase[index] - std::arg(response[evidence.bin[index]]);
        total += evidence.weight[index];
    }
    double best = low, bestScore = -1e300;
    for (double offset = low; offset <= high; offset += 0.5) {
        double score = 0;
        for (std::size_t index = 0; index < evidence.count; ++index) score += evidence.weight[index] * std::cos(phase[index] - evidence.omega[index] * offset);
        if (score > bestScore) { bestScore = score; best = offset; }
    }
    agreement = total > 0 ? bestScore / total : 0;
    for (int pass = 0; pass < 3; ++pass) {
        double numerator = 0, denominator = 0;
        for (std::size_t index = 0; index < evidence.count; ++index) {
            const double rest = std::remainder(phase[index] - evidence.omega[index] * best, 2 * kPi);
            if (std::abs(rest) > 1.2) continue;
            numerator += evidence.weight[index] * evidence.omega[index] * rest;
            denominator += evidence.weight[index] * evidence.omega[index] * evidence.omega[index];
        }
        if (!(denominator > 0)) break;
        best += numerator / denominator;
    }
    return best;
}

std::optional<WindModel::Delay> WindModel::spectralDelay() const {
    if (!ready() || !fitted_) return std::nullopt;
    Evidence evidence;
    gather(short_, evidence);
    double information = 0;
    for (std::size_t index = 0; index < evidence.count; ++index) information += evidence.weight[index] * evidence.omega[index] * evidence.omega[index];
    if (static_cast<double>(evidence.count) < kMinEvidence || information < kDelayMinInformation) return std::nullopt;
    Response h;
    responses(cutoff_, h);
    double agreement = 0;
    const double offset = delayOf(evidence, h.data(), -kTrackRadius, kTrackRadius, agreement);
    if (agreement < kDelayMinAgreement || std::abs(offset) > kTrackRadius) return std::nullopt;
    return Delay{offset, 1 / information};
}

std::optional<double> WindModel::highBandDelay() {
    if (highFilled_ < kHistory) return std::nullopt;
    static const WindInterpolator interpolator;
    std::array<std::array<double, kLags>, 2> correlation{};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        std::array<float, kHistory> x, y;
        for (std::size_t index = 0; index < kHistory; ++index) {
            x[index] = highInput_[channel][(highPosition_ + index) % kHistory];
            y[index] = highAligned_[channel][(highPosition_ + index) % kHistory];
        }
        double inputEnergy = 0;
        for (std::size_t n = 0; n < kTrackFrames; ++n) inputEnergy += static_cast<double>(x[kLagSpan + n]) * x[kLagSpan + n];
        for (std::size_t lag = 0; lag < kLags; ++lag) {
            float cross = 0, energy = 0;
            for (std::size_t n = 0; n < kTrackFrames; ++n) {
                cross += x[kLagSpan + n] * y[lag + n];
                energy += y[lag + n] * y[lag + n];
            }
            const double product = inputEnergy * energy;
            correlation[channel][lag] = product <= 1e-48 ? 0 : cross / std::sqrt(product);
        }
    }
    const auto larger = [&](int lag) { return std::max(correlation[0][static_cast<std::size_t>(lag + kLagSpan)], correlation[1][static_cast<std::size_t>(lag + kLagSpan)]); };
    int coarse = 0;
    for (int lag = -kTrackRadius; lag <= kTrackRadius; ++lag) if (larger(lag) > larger(coarse)) coarse = lag;
    // Interpolating the correlation equals correlating with an interpolated reference.
    const auto fine = [&](double offset) {
        const double whole = std::floor(offset);
        const auto phase = static_cast<std::size_t>((offset - whole) * static_cast<double>(kSincPhases) + 0.5);
        const float* taps = interpolator.phase(phase);
        double lowest = 1;
        for (const auto& channel : correlation) {
            double sum = 0;
            for (std::size_t tap = 0; tap < kSincTaps; ++tap) {
                const int lag = static_cast<int>(whole) + static_cast<int>(tap) - static_cast<int>(kSincTaps) / 2 + 1;
                if (lag >= -kLagSpan && lag <= kLagSpan) sum += taps[tap] * channel[static_cast<std::size_t>(lag + kLagSpan)];
            }
            lowest = std::min(lowest, sum);
        }
        return lowest;
    };
    std::array<double, kTrackFineSteps + 1> values{};
    std::size_t best = 0;
    for (std::size_t step = 0; step <= kTrackFineSteps; ++step) {
        values[step] = fine(coarse - 0.5 + static_cast<double>(step) / static_cast<double>(kTrackFineSteps));
        if (values[step] > values[best]) best = step;
    }
    double refinement = 0;
    if (best > 0 && best < kTrackFineSteps) {
        const double curve = values[best - 1] - 2 * values[best] + values[best + 1];
        if (curve < -1e-12) refinement = std::clamp(0.5 * (values[best - 1] - values[best + 1]) / curve, -0.5, 0.5);
    }
    double sidelobe = 0;
    for (int lag = -kTrackRadius; lag <= kTrackRadius; ++lag) if (std::abs(lag - coarse) >= 2) sidelobe = std::max(sidelobe, larger(lag));
    if (values[best] < kTrackPeak || values[best] - sidelobe < kTrackSidelobe) return std::nullopt;
    return coarse - 0.5 + (static_cast<double>(best) + refinement) / static_cast<double>(kTrackFineSteps);
}
} // namespace hikari
