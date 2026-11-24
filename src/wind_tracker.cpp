// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "wind_tracker.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hikari {
using namespace wind;
namespace {
// The position loop hears from the model once per independent spectrum window.
constexpr std::uint64_t kLoopBlocks = 16;
constexpr double kSpectralAge = 3072, kHighBandAge = 2077;
constexpr double kSpectralFloor = 0.02 * 0.02, kHighBandVariance = 0.05 * 0.05;
constexpr double kSpectralInflation = 2;
constexpr std::int64_t kDelayStale = 96000;
constexpr std::size_t kBackfill = 47616;
static_assert(kBackfill % kBlock == 0);
constexpr int kDropEstimates = 3;
constexpr std::size_t kRelocateFrames = 24000;
// After a lock broke off at once: when to look for it close by, how often, and for how long.
constexpr std::int64_t kRecoverFirst = 9600, kRecoverEvery = 4608, kRecoverLast = 28800;
constexpr int kRecoverBands = 2;
constexpr std::int64_t kFitPatience = 96000;
// Sub-frames above this multiple of the usual power are turned down to it.
constexpr double kLoudPower = 4;
constexpr std::size_t kMinKept = 20;
// How far a segment may move away from its end of the window, and by how much calmer that has to be.
constexpr std::size_t kSegmentReach = 11520;
constexpr double kCalmer = 0.5;
constexpr int kMaxUnclear = 4;

double parabola(double left, double center, double right) noexcept {
    const double curve = left - 2 * center + right;
    return curve < -1e-12 ? std::clamp(0.5 * (left - right) / curve, -0.5, 0.5) : 0;
}
}

void WindTracker::Loop::reset() noexcept { pp = 9; pr = 0; rr = 1e-8; }

void WindTracker::Loop::predict(double frames) noexcept {
    pp += 2 * frames * pr + frames * frames * rr + 1e-6;
    pr += frames * rr;
    rr += 1e-14;
}

// `age` is how long ago, on average, the offset was observed.
void WindTracker::Loop::correct(double offset, double variance, double age, double& position, double& rate) noexcept {
    const double hp = pp - age * pr, hr = pr - age * rr;
    const double total = hp - age * hr + variance;
    if (!(total > 0)) return;
    const double kp = hp / total, kr = hr / total;
    position += kp * offset;
    rate += kr * offset;
    pp -= kp * hp;
    pr -= kp * hr;
    rr -= kr * hr;
    pp = std::max(pp, 1e-8);
    rr = std::max(rr, 1e-16);
}

WindTracker::WindTracker(std::shared_ptr<const WindReferenceSet> references)
    : references_(std::move(references)), ring_(kRing * 2, 0.0f), inputBlock_(kBlock * 2), alignedBlock_(kBlock * 2) {
    if (!references_ || references_->items().empty()) throw std::invalid_argument("Wind references are required");
    scratch_.resize((kWindow + 64) * 2);
    referenceScratch_.resize((kWindow + 2 * kSegmentRadius + 64) * 2);
    for (auto& slot : slots_) {
        slot.input.resize(kConfirmFrames * 2);
        slot.aligned.resize(kConfirmFrames * 2);
    }
}

void WindTracker::copyInput(std::int64_t first, std::size_t frames, float* stereo) const noexcept {
    for (std::size_t done = 0; done < frames;) {
        const auto slot = static_cast<std::size_t>(first + static_cast<std::int64_t>(done)) & (kRing - 1);
        const std::size_t run = std::min(frames - done, kRing - slot);
        std::copy_n(ring_.data() + slot * 2, run * 2, stereo + done * 2);
        done += run;
    }
}

void WindTracker::copyCalm(std::int64_t first, std::size_t frames, float* stereo) const noexcept {
    copyInput(first, frames, stereo);
    for (std::size_t frame = 0; frame < frames;) {
        const std::int64_t index = (first + static_cast<std::int64_t>(frame)) / static_cast<std::int64_t>(kSubFrame);
        const std::int64_t end = (index + 1) * static_cast<std::int64_t>(kSubFrame) - first;
        const std::size_t stop = std::min(frames, static_cast<std::size_t>(end));
        const double power = powers_[static_cast<std::size_t>(index) % kPowers];
        if (usualPower_ > 0 && power > kLoudPower * usualPower_) {
            const auto scale = static_cast<float>(std::sqrt(kLoudPower * usualPower_ / power));
            for (std::size_t at = frame * 2; at < stop * 2; ++at) stereo[at] *= scale;
        }
        frame = stop;
    }
}

void WindTracker::push(const float* stereo, std::size_t frames) {
    for (std::size_t done = 0; done < frames;) {
        const auto position = static_cast<std::size_t>(now_);
        std::size_t run = std::min(frames - done, std::min(kBlock - position % kBlock, kSubFrame - position % kSubFrame));
        const std::size_t slot = position & (kRing - 1);
        run = std::min(run, kRing - slot);
        std::copy_n(stereo + done * 2, run * 2, ring_.data() + slot * 2);
        now_ += static_cast<std::int64_t>(run);
        done += run;
        if (static_cast<std::size_t>(now_) % kBlock == 0) block();
        if (static_cast<std::size_t>(now_) % kSubFrame == 0) subFrame();
    }
}

void WindTracker::discontinuity(std::uint32_t breaks) {
    breaks_ = breaks;
    subFrames_ = 0;
    for (auto& slot : slots_) slot.used = false;
    recoverFrom_ = -1;
    if (lock_.locked) relocateFrom_ = now_;
    else last_.valid = false;
}

void WindTracker::subFrame() {
    copyInput(now_ - static_cast<std::int64_t>(kSubFrame), kSubFrame, scratch_.data());
    const auto energies = fingerprint_.analyze(scratch_.data());
    const auto slot = static_cast<std::size_t>(subFrames_ % kSubFrames);
    for (std::size_t band = 0; band < kBands; ++band) energies_[band][slot] = energies[band];
    double power = 0;
    for (std::size_t index = 0; index < kSubFrame * 2; ++index) power += static_cast<double>(scratch_[index]) * scratch_[index];
    powers_[static_cast<std::size_t>(now_ / static_cast<std::int64_t>(kSubFrame) - 1) % kPowers] = power / static_cast<double>(kSubFrame);
    ++subFrames_;
    if (!lock_.locked && power / static_cast<double>(kSubFrame) > kHeardPower) searched_ += static_cast<std::int64_t>(kSubFrame);
    if (lock_.locked || subFrames_ < kSubFrames) return;
    const bool recent = last_.valid && now_ - last_.frame < kRecentLockFrames;
    if (now_ - lastSearch_ < static_cast<std::int64_t>(recent ? kSearchInterval / 2 : kSearchInterval)) return;
    lastSearch_ = now_;
    search();
}

void WindTracker::updateUsualPower() {
    std::array<double, kSubFrames> recent{};
    const std::int64_t last = now_ / static_cast<std::int64_t>(kSubFrame);
    double sum = 0;
    for (std::size_t index = 0; index < kSubFrames; ++index) {
        recent[index] = powers_[static_cast<std::size_t>(last - 1 - static_cast<std::int64_t>(index)) % kPowers];
        sum += recent[index];
    }
    meanPower_ = sum / static_cast<double>(kSubFrames);
    std::nth_element(recent.begin(), recent.begin() + kSubFrames / 2, recent.end());
    usualPower_ = recent[kSubFrames / 2];
}

bool WindTracker::present() const {
    if (meanPower_ < kPresencePower) return false;
    std::array<double, kBands> mean{};
    for (std::size_t band = 0; band < kBands; ++band) {
        for (std::size_t index = 0; index < kSubFrames; ++index)
            mean[band] += static_cast<double>(mask_[index]) * std::exp(static_cast<double>(energies_[band][(subFrames_ + index) % kSubFrames]));
        mean[band] /= kept_;
    }
    const auto profile = windProfile(mean);
    for (const auto& reference : references_->items()) {
        double similarity = 0;
        for (std::size_t band = 0; band < kBands; ++band) similarity += profile[band] * reference.profile[band];
        if (similarity >= kPresenceCosine) return true;
    }
    return false;
}

void WindTracker::search() {
    ++searches_;
    updateUsualPower();
    // Loud sub-frames are left out of the comparison, unless that would leave too little to compare.
    std::size_t kept = 0;
    const std::int64_t last = now_ / static_cast<std::int64_t>(kSubFrame);
    for (std::size_t index = 0; index < kSubFrames; ++index) {
        const auto power = powers_[static_cast<std::size_t>(last - static_cast<std::int64_t>(kSubFrames - index)) % kPowers];
        mask_[index] = power <= kLoudPower * usualPower_ ? 1.0f : 0.0f;
        kept += mask_[index] > 0 ? 1 : 0;
    }
    if (kept < kMinKept) { mask_.fill(1); kept = kSubFrames; }
    kept_ = static_cast<double>(kept);
    if (!present()) return;
    for (std::size_t band = 0; band < kBands; ++band) {
        double mean = 0, square = 0;
        for (std::size_t index = 0; index < kSubFrames; ++index) {
            window_[band][index] = energies_[band][(subFrames_ + index) % kSubFrames];
            mean += static_cast<double>(mask_[index]) * window_[band][index];
        }
        mean /= static_cast<double>(kept);
        for (std::size_t index = 0; index < kSubFrames; ++index) {
            auto& value = window_[band][index];
            value = static_cast<float>((value - mean) * mask_[index]);
            square += static_cast<double>(value) * value;
        }
        const double scale = 1 / std::sqrt(std::max(kNormFloor, square));
        for (auto& value : window_[band]) value = static_cast<float>(value * scale);
    }
    const std::int64_t frame = now_ - static_cast<std::int64_t>(kWindow);
    std::vector<Candidate> candidates;
    for (std::size_t reference = 0; reference < references_->items().size(); ++reference) scan(reference, candidates);
    for (auto& candidate : candidates) {
        coarse(candidate.reference, frame, kWindow / kCoarseDecimation, kCoarseRadius, candidate.position);
        candidate.score = std::clamp(fine(candidate.reference, frame, candidate.position), 0.0, 1.0);
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (candidates.empty() || candidates.front().score < kAlignScore || mono(frame)) return;
    const std::size_t tries = candidates.front().score < kTryTwoScore ? 1 : 2;
    for (std::size_t index = 0; index < std::min(tries, candidates.size()); ++index) {
        const auto alignment = align(candidates[index].reference, frame, candidates[index].position);
        if (!alignment) continue;
        consider(candidates[index].reference, *alignment);
        if (lock_.locked) return;
    }
}

void WindTracker::scan(std::size_t index, std::vector<Candidate>& output) {
    const auto& reference = references_->items()[index];
    scores_.assign(reference.hops, 0.0);
    for (std::size_t band = 0; band < kBands; ++band) {
        const float* values = reference.band(band);
        for (std::size_t hop = 0; hop < reference.hops; ++hop) {
            float cross = 0, sum = 0, square = 0;
            for (std::size_t step = 0; step < kSubFrames; ++step) {
                const float value = values[hop + step], masked = value * mask_[step];
                cross += value * window_[band][step];
                sum += masked;
                square += masked * value;
            }
            scores_[hop] += cross / std::sqrt(std::max(kNormFloor, square - static_cast<double>(sum) * sum / kept_));
        }
    }
    std::vector<std::size_t> peaks;
    for (std::size_t pick = 0; pick < kMaxPeaks; ++pick) {
        std::size_t best = reference.hops;
        for (std::size_t hop = 0; hop < reference.hops; ++hop) {
            if (best != reference.hops && scores_[hop] <= scores_[best]) continue;
            const bool taken = std::any_of(peaks.begin(), peaks.end(), [&](std::size_t peak) {
                const std::size_t direct = hop > peak ? hop - peak : peak - hop;
                return std::min(direct, reference.hops - direct) * kSubFrame < kPeakDistance;
            });
            if (!taken) best = hop;
        }
        if (best == reference.hops) break;
        peaks.push_back(best);
        output.push_back({index, static_cast<double>(best * kSubFrame), scores_[best] / static_cast<double>(kBands)});
    }
}

// Box-averaged correlation around `position`; moves it by a multiple of the decimation.
bool WindTracker::coarse(std::size_t index, std::int64_t frame, std::size_t points, std::size_t radius, double& position) const {
    const auto& reference = references_->items()[index];
    const std::size_t lags = 2 * (radius / kCoarseDecimation) + 1, length = points + lags - 1;
    std::vector<float> input(points * 2), target(length * 2), raw(kCoarseDecimation * 2);
    for (std::size_t point = 0; point < points; ++point) {
        copyCalm(frame + static_cast<std::int64_t>(point * kCoarseDecimation), kCoarseDecimation, raw.data());
        for (std::size_t channel = 0; channel < 2; ++channel) {
            float sum = 0;
            for (std::size_t step = 0; step < kCoarseDecimation; ++step) sum += raw[step * 2 + channel];
            input[point + channel * points] = sum / static_cast<float>(kCoarseDecimation);
        }
    }
    const double start = reference.wrap(std::floor(position) - static_cast<double>(radius));
    std::vector<float> source(length * kCoarseDecimation * 2);
    reference.linear(start, length * kCoarseDecimation, source.data());
    for (std::size_t point = 0; point < length; ++point) for (std::size_t channel = 0; channel < 2; ++channel) {
        float sum = 0;
        for (std::size_t step = 0; step < kCoarseDecimation; ++step) sum += source[(point * kCoarseDecimation + step) * 2 + channel];
        target[point + channel * length] = sum / static_cast<float>(kCoarseDecimation);
    }
    double best = -1;
    std::size_t bestLag = lags / 2;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const float* x = input.data() + channel * points;
        const float* y = target.data() + channel * length;
        double sx = 0, sxx = 0, sy = 0, syy = 0;
        for (std::size_t point = 0; point < points; ++point) {
            sx += x[point]; sxx += static_cast<double>(x[point]) * x[point];
            sy += y[point]; syy += static_cast<double>(y[point]) * y[point];
        }
        const double count = static_cast<double>(points), variance = sxx - sx * sx / count;
        for (std::size_t lag = 0; lag < lags; ++lag) {
            float cross = 0;
            for (std::size_t point = 0; point < points; ++point) cross += x[point] * y[lag + point];
            const double product = variance * (syy - sy * sy / count);
            const double value = product > 1e-30 ? std::abs(cross - sx * sy / count) / std::sqrt(product) : 0;
            if (value > best) { best = value; bestLag = lag; }
            if (lag + 1 == lags) break;
            const double leaving = y[lag], entering = y[lag + points];
            sy += entering - leaving;
            syy += entering * entering - leaving * leaving;
        }
    }
    position = reference.wrap(start + static_cast<double>(bestLag * kCoarseDecimation));
    return best > 0;
}

// Sample-accurate correlation of the raw tail; returns the larger channel's magnitude.
double WindTracker::fine(std::size_t index, std::int64_t frame, double& position) const {
    const auto& reference = references_->items()[index];
    constexpr std::size_t lags = 2 * kFineRadius + 1;
    std::vector<float> input(kRawTail * 2), target((kRawTail + lags - 1) * 2);
    copyCalm(frame + static_cast<std::int64_t>(kRawTailStart), kRawTail, input.data());
    reference.linear(position + static_cast<double>(kRawTailStart) - static_cast<double>(kFineRadius), kRawTail + lags - 1, target.data());
    std::array<std::array<double, lags>, 2> values{};
    double best = -1;
    std::size_t bestLag = kFineRadius, bestChannel = 0;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        double sx = 0, sxx = 0;
        for (std::size_t n = 0; n < kRawTail; ++n) { const double x = input[n * 2 + channel]; sx += x; sxx += x * x; }
        const double count = static_cast<double>(kRawTail), variance = sxx - sx * sx / count;
        for (std::size_t lag = 0; lag < lags; ++lag) {
            double cross = 0, sy = 0, syy = 0;
            for (std::size_t n = 0; n < kRawTail; ++n) {
                const double y = target[(lag + n) * 2 + channel];
                cross += input[n * 2 + channel] * y; sy += y; syy += y * y;
            }
            const double product = variance * (syy - sy * sy / count);
            values[channel][lag] = product > 1e-30 ? std::abs(cross - sx * sy / count) / std::sqrt(product) : 0;
            if (values[channel][lag] > best) { best = values[channel][lag]; bestLag = lag; bestChannel = channel; }
        }
    }
    double shift = 0;
    if (bestLag > 0 && bestLag + 1 < lags) shift = parabola(values[bestChannel][bestLag - 1], best, values[bestChannel][bestLag + 1]);
    position = reference.wrap(position + static_cast<double>(bestLag) - static_cast<double>(kFineRadius) + shift);
    return best;
}

// Both channels together; moves `position` (the reference position of `frame`) to the peak.
double WindTracker::correlate(std::size_t index, std::int64_t frame, std::size_t frames, std::size_t radius, double& position) {
    const auto& reference = references_->items()[index];
    const std::size_t lags = 2 * radius + 1;
    copyCalm(frame, frames, scratch_.data());
    reference.linear(position - static_cast<double>(radius), frames + lags - 1, referenceScratch_.data());
    const std::size_t count = frames * 2;
    double sx = 0, sxx = 0;
    for (std::size_t n = 0; n < count; ++n) { sx += scratch_[n]; sxx += static_cast<double>(scratch_[n]) * scratch_[n]; }
    const double total = static_cast<double>(count), variance = sxx - sx * sx / total;
    std::array<double, 2 * kSegmentRadius + 1> values{};
    std::size_t best = radius;
    for (std::size_t lag = 0; lag < lags; ++lag) {
        const float* target = referenceScratch_.data() + lag * 2;
        double cross = 0, sy = 0, syy = 0;
        for (std::size_t n = 0; n < count; ++n) {
            const double y = target[n];
            cross += scratch_[n] * y; sy += y; syy += y * y;
        }
        const double product = variance * (syy - sy * sy / total);
        values[lag] = product > 1e-30 ? std::abs(cross - sx * sy / total) / std::sqrt(product) : 0;
        if (values[lag] > values[best]) best = lag;
    }
    double shift = 0;
    if (best > 0 && best + 1 < lags) shift = parabola(values[best - 1], values[best], values[best + 1]);
    position += static_cast<double>(best) - static_cast<double>(radius) + shift;
    return values[best];
}

std::optional<WindTracker::Alignment> WindTracker::align(std::size_t index, std::int64_t frame, double position) {
    const auto& reference = references_->items()[index];
    double whole = position;
    const double all = correlate(index, frame, kWindow, kWholeRadius, whole);
    // The calmest stretch near each end of the window; with nothing loud around these are the two ends.
    const auto calmest = [&](std::size_t from, std::size_t to, bool late) {
        std::size_t best = late ? to : from;
        double lowest = -1;
        for (std::size_t step = 0; from + step <= to; step += kSubFrame) {
            const std::size_t start = late ? to - step : from + step;
            double power = 0;
            for (std::size_t at = 0; at < kSegment; at += kSubFrame)
                power += powers_[static_cast<std::size_t>((frame + static_cast<std::int64_t>(start + at)) / static_cast<std::int64_t>(kSubFrame)) % kPowers];
            if (lowest < 0 || power < kCalmer * lowest) { lowest = power; best = start; }
        }
        return best;
    };
    const std::size_t a = calmest(0, kSegmentReach, false), b = calmest(kSegmentSpan - kSegmentReach, kSegmentSpan, true);
    double first = whole + static_cast<double>(a), second = whole + static_cast<double>(b);
    const double early = correlate(index, frame + static_cast<std::int64_t>(a), kSegment, kSegmentRadius, first);
    const double late = correlate(index, frame + static_cast<std::int64_t>(b), kSegment, kSegmentRadius, second);
    const double weakest = std::min(all, std::min(early, late));
    if (weakest < kLooseCorrelation) return std::nullopt;
    const double span = static_cast<double>(b - a);
    const double rate = 1 + reference.distance(second - first, span) / span;
    if (std::abs(rate - 1) > kMaxRateError * static_cast<double>(kSegmentSpan) / span) return std::nullopt;
    Alignment result;
    // Each segment estimates the position of its own middle.
    const double middle = static_cast<double>(kSegment) / 2;
    result.frame = frame + static_cast<std::int64_t>(b + kSegment / 2);
    result.position = reference.wrap(second + middle);
    result.rate = std::clamp(rate, 1 - kMaxRateError, 1 + kMaxRateError);
    result.correlation = weakest;
    return result;
}

bool WindTracker::mono(std::int64_t frame) {
    copyInput(frame, kWindow, scratch_.data());
    double difference = 0, power = 0;
    for (std::size_t n = 0; n < kWindow; ++n) {
        const double left = scratch_[n * 2], right = scratch_[n * 2 + 1];
        difference += (left - right) * (left - right);
        power += left * left + right * right;
    }
    return difference < std::max(kMonoFloor, kMonoRatio * power);
}

void WindTracker::consider(std::size_t index, const Alignment& alignment) {
    const auto& reference = references_->items()[index];
    const auto elapsed = now_ - last_.frame;
    if (last_.valid && last_.reference == index && elapsed < kRecentLockFrames) {
        const double predicted = last_.position + static_cast<double>(alignment.frame - last_.frame) * last_.rate;
        const double distance = std::abs(reference.distance(alignment.position, predicted));
        const double drift = kHandoverPerFrame * static_cast<double>(elapsed);
        // Right after a lock a jump is far more likely a false match than a restart of the loop, unless the lock
        // broke off at once: then it did jump, and it has to match well to be believed.
        if (distance >= kRecentLockDistance + drift && !strict()) return;
        if (distance <= kHandoverRadius + drift && !strict()) {
            beginLock(index, alignment.frame, alignment.position, alignment.rate, true);
            return;
        }
    }
    for (auto& slot : slots_) {
        if (!slot.used || slot.reference != index) continue;
        const double predicted = slot.position + static_cast<double>(alignment.frame - slot.frame) * slot.rate;
        if (std::abs(reference.distance(alignment.position, predicted)) < kHandoverRadius) return;
    }
    for (auto& slot : slots_) {
        if (slot.used) continue;
        slot.used = true;
        slot.reference = index;
        slot.rate = alignment.rate;
        slot.frame = now_;
        slot.position = reference.wrap(alignment.position + static_cast<double>(now_ - alignment.frame) * alignment.rate);
        slot.passes = slot.unclear = 0;
        slot.filled = 0;
        slot.weakest = 1;
        slot.inputFilter = {};
        slot.referenceFilter = {};
        return;
    }
}

void WindTracker::readAligned(std::size_t index, double position, double rate, std::size_t frames, float* stereo) const noexcept {
    const auto& reference = references_->items()[index];
    const auto& interpolator = references_->interpolator();
    for (std::size_t frame = 0; frame < frames; ++frame)
        interpolator.read(reference, position + static_cast<double>(frame) * rate, stereo[frame * 2], stereo[frame * 2 + 1]);
}

void WindTracker::runSlot(Slot& slot) {
    // A slot opened between two block boundaries starts with its first whole block.
    const std::int64_t first = now_ - static_cast<std::int64_t>(kBlock);
    const auto& reference = references_->items()[slot.reference];
    const double position = slot.position + static_cast<double>(first - slot.frame) * slot.rate;
    if (first < slot.frame) return;
    copyInput(first, kBlock, inputBlock_.data());
    readAligned(slot.reference, position, slot.rate, kBlock, alignedBlock_.data());
    for (std::size_t frame = 0; frame < kBlock; ++frame) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            slot.input[(slot.filled + frame) * 2 + channel] = slot.inputFilter.process(channel, inputBlock_[frame * 2 + channel]);
            slot.aligned[(slot.filled + frame) * 2 + channel] = slot.referenceFilter.process(channel, alignedBlock_[frame * 2 + channel]);
        }
    }
    slot.filled += kBlock;
    slot.frame = now_;
    slot.position = reference.wrap(position + static_cast<double>(kBlock) * slot.rate);
    if (slot.filled < kConfirmFrames) return;
    slot.filled = 0;
    const auto verdict = confirm(slot);
    if (verdict == Verdict::contradicted || (verdict == Verdict::unclear && ++slot.unclear > kMaxUnclear)) { slot.used = false; return; }
    if (verdict == Verdict::unclear || ++slot.passes < kConfirmPasses) return;
    slot.used = false;
    // Other sounds lower the correlation, but not what the fitted model explains.
    if (strict() && slot.weakest < kQualityReturn && !explains(slot.reference, {slot.frame, slot.position, slot.rate, 0})) return;
    beginLock(slot.reference, slot.frame, slot.position, slot.rate, false);
}

WindTracker::Verdict WindTracker::confirm(Slot& slot) {
    // Blocks far louder than the usual one are turned down before the comparison, as everywhere else.
    constexpr std::size_t blocks = kConfirmFrames / kBlock;
    std::array<double, blocks> energy{};
    for (std::size_t block = 0; block < blocks; ++block)
        for (std::size_t at = block * kBlock * 2; at < (block + 1) * kBlock * 2; ++at) energy[block] += static_cast<double>(slot.input[at]) * slot.input[at];
    auto sorted = energy;
    std::nth_element(sorted.begin(), sorted.begin() + blocks / 2, sorted.end());
    const double limit = kLoudPower * sorted[blocks / 2];
    for (std::size_t block = 0; block < blocks; ++block) {
        if (!(limit > 0) || energy[block] <= limit) continue;
        const auto scale = static_cast<float>(std::sqrt(limit / energy[block]));
        for (std::size_t at = block * kBlock * 2; at < (block + 1) * kBlock * 2; ++at) slot.input[at] *= scale;
    }
    double weakest = 1;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto correlation = [&](int lag) {
            double cross = 0, xx = 0, yy = 0;
            const auto begin = static_cast<int>(kConfirmWarmup) + kConfirmLags.back();
            const auto end = static_cast<int>(kConfirmFrames) - kConfirmLags.back();
            for (int n = begin; n < end; ++n) {
                const double x = slot.input[static_cast<std::size_t>(n) * 2 + channel];
                const double y = slot.aligned[static_cast<std::size_t>(n + lag) * 2 + channel];
                cross += x * y; xx += x * x; yy += y * y;
            }
            return cross / std::sqrt(std::max(1e-48, xx * yy));
        };
        const double center = correlation(0);
        double side = -1;
        for (const int lag : kConfirmLags) side = std::max(side, std::max(correlation(lag), correlation(-lag)));
        // Drowned out in this window: neither a pass nor a reason to start over.
        if (std::max(center, side) < kConfirmCorrelation) { weakest = 0; continue; }
        if (center - side < kConfirmMargin) return Verdict::contradicted;
        weakest = std::min(weakest, center);
    }
    if (weakest < kConfirmCorrelation) return Verdict::unclear;
    slot.weakest = std::min(slot.weakest, weakest);
    return Verdict::passed;
}

// The last stretch of input against the aligned reference, so that the model starts with full statistics.
void WindTracker::backfill() {
    model_.resetStatistics();
    for (std::int64_t at = lock_.frame - static_cast<std::int64_t>(kBackfill); at < lock_.frame; at += static_cast<std::int64_t>(kBlock)) {
        copyInput(at, kBlock, inputBlock_.data());
        readAligned(static_cast<std::size_t>(lock_.reference), positionAt(at), lock_.rate, kBlock, alignedBlock_.data());
        model_.feed(inputBlock_.data(), alignedBlock_.data(), kBlock);
    }
}

bool WindTracker::beginModel() {
    const auto offset = model_.begin();
    if (!offset) return false;
    move(*offset);
    backfill();
    apparent_ = model_.apparentDelay();
    model_.estimate();
    return true;
}

void WindTracker::beginLock(std::size_t reference, std::int64_t frame, double position, double rate, bool continued) {
    for (auto& slot : slots_) slot.used = false;
    relocateFrom_ = recoverFrom_ = -1;
    strictReturn_ = false;
    dropCount_ = 0;
    const bool keep = continued && model_.fitted();
    const std::int64_t boundary = now_ - now_ % static_cast<std::int64_t>(kBlock);
    lock_.epoch += 1;
    lock_.breaks = breaks_;
    searched_ = 0;
    lock_.reference = static_cast<int>(reference);
    lock_.rate = rate;
    lock_.frame = boundary;
    lock_.position = references_->items()[reference].wrap(position + (keep ? apparent_ : 0) + static_cast<double>(boundary - frame) * rate);
    lock_.locked = true;
    lock_.continued = keep;
    lock_.abrupt = false;
    loop_.reset();
    if (!keep) { model_.reset(); apparent_ = 0; }
    bad_.fill(0);
    badPosition_ = badCount_ = 0;
    quality_ = 1;
    lockedAt_ = lastRealign_ = lastDelay_ = now_;
    backfill();
    if (keep) model_.estimate();
    else beginModel();
    publish();
}

void WindTracker::loseLock(bool abrupt) {
    if (!lock_.locked) return;
    last_.valid = true;
    last_.reference = static_cast<std::size_t>(lock_.reference);
    last_.frame = lock_.frame;
    last_.position = lock_.position - apparent_;
    last_.rate = lock_.rate;
    lock_.locked = false;
    lock_.abrupt = abrupt;
    relocateFrom_ = -1;
    ++losses_;
    publish();
}

void WindTracker::publish() {
    lock_.gain = model_.gain();
    lock_.cutoff = model_.cutoff();
    lock_.presence = model_.presence();
    ++revision_;
}

void WindTracker::block() {
    ++blocks_;
    if (!lock_.locked) {
        for (auto& slot : slots_) {
            if (slot.used) runSlot(slot);
            if (lock_.locked) return;
        }
        if (recoverFrom_ >= 0) recover();
        return;
    }
    if (relocateFrom_ >= 0) {
        lock_.position = references_->items()[static_cast<std::size_t>(lock_.reference)].wrap(positionAt(now_));
        lock_.frame = now_;
        if (now_ - relocateFrom_ >= static_cast<std::int64_t>(kRelocateFrames)) relocate();
        return;
    }
    trackBlock();
}

void WindTracker::trackBlock() {
    const auto index = static_cast<std::size_t>(lock_.reference);
    const auto& reference = references_->items()[index];
    const std::int64_t first = now_ - static_cast<std::int64_t>(kBlock);
    copyInput(first, kBlock, inputBlock_.data());
    readAligned(index, positionAt(first), lock_.rate, kBlock, alignedBlock_.data());
    model_.feed(inputBlock_.data(), alignedBlock_.data(), kBlock);
    lock_.position = reference.wrap(positionAt(now_));
    lock_.frame = now_;

    if (!model_.fitted()) {
        // Nothing is subtracted before the first fit; without one this lock is of no use.
        if (blocks_ % kLoopBlocks != 0) return;
        if (beginModel()) publish();
        else if (now_ - lockedAt_ > kFitPatience) loseLock(false);
        return;
    }
    quality_ = model_.quality();
    const unsigned char bad = std::abs(quality_) <= kQualityBad ? 1 : 0;
    badCount_ += bad;
    badCount_ -= bad_[badPosition_];
    bad_[badPosition_] = bad;
    badPosition_ = badPosition_ + 1 == kQualityBlocks ? 0 : badPosition_ + 1;
    if (badCount_ > kQualityMaxBad) { loseLock(false); return; }

    if (blocks_ % kTrackEveryBlocks != 0) return;
    model_.estimate();
    apparent_ = model_.apparentDelay();
    const auto bands = model_.bandQualities();
    int reliable = 0, gone = 0;
    for (const auto& band : bands) {
        if (!band.reliable) continue;
        ++reliable;
        if (band.value < kQualityDrop) ++gone;
    }
    // Every band that can be judged has to agree; one quiet band alone is ordinary in a busy mix.
    dropCount_ = reliable >= 2 && gone == reliable ? dropCount_ + 1 : 0;
    if (dropCount_ >= kDropEstimates) {
        loseLock(false);
        strictReturn_ = true;
        recoverFrom_ = now_;
        return;
    }
    if (blocks_ % kLoopBlocks == 0) trackPosition();
    if (lock_.locked && now_ - lastRealign_ >= kRealignFrames) realign();
    publish();
}

void WindTracker::trackPosition() {
    loop_.predict(static_cast<double>(kLoopBlocks * kBlock));
    double offset = 0, variance = 0, age = 0;
    if (const auto high = model_.highBandDelay()) {
        offset = *high;
        variance = kHighBandVariance;
        age = kHighBandAge;
    } else if (const auto spectral = model_.spectralDelay()) {
        offset = spectral->offset;
        variance = spectral->variance * kSpectralInflation + kSpectralFloor;
        age = kSpectralAge;
    } else return;
    lastDelay_ = now_;
    double position = lock_.position, rate = lock_.rate;
    loop_.correct(offset, variance, age, position, rate);
    lock_.rate = std::clamp(rate, 1 - kMaxRateError, 1 + kMaxRateError);
    move(position - lock_.position);
}

void WindTracker::move(double offset) {
    const auto& reference = references_->items()[static_cast<std::size_t>(lock_.reference)];
    lock_.position = reference.wrap(lock_.position + offset);
    if (std::abs(offset) >= kResetOffset) model_.resetStatistics();
}

// A slow safety net: without any delay evidence for a while, or with a clear slip, take the direct alignment.
void WindTracker::realign() {
    lastRealign_ = now_;
    updateUsualPower();
    const auto index = static_cast<std::size_t>(lock_.reference);
    const std::int64_t frame = now_ - static_cast<std::int64_t>(kWindow);
    const auto alignment = align(index, frame, positionAt(frame) - apparent_);
    if (!alignment) return;
    const auto& reference = references_->items()[index];
    const double offset = reference.distance(alignment->position + apparent_, positionAt(alignment->frame));
    if (std::abs(offset) > kHandoverRadius) return;
    if (now_ - lastDelay_ < kDelayStale ? std::abs(offset) < kRecentLockDistance : std::abs(offset) < kJumpOffset) return;
    lock_.rate = alignment->rate;
    loop_.reset();
    move(offset);
}

void WindTracker::relocate() {
    // After a break of unknown length the old position says nothing about the next one.
    const auto lose = [this] { loseLock(true); last_.valid = false; };
    const auto index = static_cast<std::size_t>(lock_.reference);
    const auto alignment = nearby(index, positionAt(now_) - apparent_, lock_.rate);
    if (!alignment) { lose(); return; }
    beginLock(index, alignment->frame, alignment->position, alignment->rate, true);
}

std::optional<WindTracker::Alignment> WindTracker::nearby(std::size_t index, double position, double rate) {
    updateUsualPower();
    const auto& reference = references_->items()[index];
    const std::size_t frames = kLocalPoints * kCoarseDecimation;
    // The power of a sub-frame is known once it is complete, and at a common boundary only after this block.
    const std::int64_t end = (now_ - 1) / static_cast<std::int64_t>(kSubFrame) * static_cast<std::int64_t>(kSubFrame);
    const std::int64_t frame = end - static_cast<std::int64_t>(frames);
    position -= static_cast<double>(now_ - frame) * rate;
    if (!coarse(index, frame, kLocalPoints, (kLocalBack - frames) / kCoarseDecimation * kCoarseDecimation, position)) return std::nullopt;
    const std::int64_t tail = end - static_cast<std::int64_t>(kRawTailStart + kRawTail);
    double precise = position + static_cast<double>(tail - frame);
    fine(index, tail, precise);
    const std::int64_t segment = end - static_cast<std::int64_t>(kSegment);
    double start = precise + static_cast<double>(segment - tail);
    Alignment result;
    result.correlation = correlate(index, segment, kSegment, kSegmentRadius, start);
    if (result.correlation < kLooseCorrelation) return std::nullopt;
    result.frame = segment + static_cast<std::int64_t>(kSegment / 2);
    result.position = reference.wrap(start + static_cast<double>(kSegment) / 2);
    result.rate = rate;
    return result;
}

// The lock broke off at once. When the sound merely slipped without anybody noticing, the same wind goes on close by.
void WindTracker::recover() {
    const std::int64_t waited = now_ - recoverFrom_;
    if (waited > kRecoverLast) { recoverFrom_ = -1; return; }
    if (waited < kRecoverFirst || (waited - kRecoverFirst) % kRecoverEvery != 0) return;
    const auto alignment = nearby(last_.reference, last_.position + static_cast<double>(now_ - last_.frame) * last_.rate, last_.rate);
    if (alignment && explains(last_.reference, *alignment)) beginLock(last_.reference, alignment->frame, alignment->position, alignment->rate, true);
}

// Whether the model fitted before explains what is heard now, at this alignment.
bool WindTracker::explains(std::size_t index, const Alignment& alignment) {
    constexpr std::size_t blocks = (kSpectrumFft + (kSpectrumFrames - 1) * kSpectrumHop + kBlock - 1) / kBlock;
    if (!model_.fitted()) return false;
    model_.resetStatistics();
    for (std::int64_t at = now_ - static_cast<std::int64_t>(blocks * kBlock); at < now_; at += static_cast<std::int64_t>(kBlock)) {
        copyInput(at, kBlock, inputBlock_.data());
        readAligned(index, alignment.position + apparent_ + static_cast<double>(at - alignment.frame) * alignment.rate, alignment.rate, kBlock, alignedBlock_.data());
        model_.feed(inputBlock_.data(), alignedBlock_.data(), kBlock);
    }
    int reliable = 0;
    for (const auto& band : model_.bandQualities()) {
        if (!band.reliable) continue;
        if (band.value < kQualityReturn) return false;
        ++reliable;
    }
    return reliable >= kRecoverBands;
}
} // namespace hikari
