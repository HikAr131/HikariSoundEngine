// SPDX-License-Identifier: AGPL-3.0-or-later
#include "graphic_eq.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
using Complex = std::complex<double>;

void designTransform(std::vector<Complex>& data, bool inverse) {
    const auto count = data.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        auto bit = count / 2;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t width = 2; width <= count; width *= 2) {
        const double angle = (inverse ? 2 : -2) * kPi / static_cast<double>(width);
        const Complex rotation(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < count; start += width) {
            Complex factor(1, 0);
            for (std::size_t offset = 0; offset < width / 2; ++offset) {
                const auto even = data[start + offset];
                const auto odd = data[start + offset + width / 2] * factor;
                data[start + offset] = even + odd;
                data[start + offset + width / 2] = even - odd;
                factor *= rotation;
            }
        }
    }
    if (inverse) for (auto& value : data) value /= static_cast<double>(count);
}

std::vector<double> designImpulse(const std::vector<GraphicBand>& points, double rate) {
    constexpr auto count = 2 * GraphicEq::kFilterTaps;
    std::vector<Complex> spectrum(count);
    for (std::size_t bin = 0; bin <= count / 2; ++bin) {
        const double frequency = static_cast<double>(bin) * rate / count;
        const double logarithm = GraphicEq::targetDb(points, frequency) * std::log(10.0) / 20;
        spectrum[bin] = logarithm;
        if (bin > 0 && bin < count / 2) spectrum[count - bin] = logarithm;
    }
    designTransform(spectrum, true);
    for (std::size_t i = 1; i < count / 2; ++i) spectrum[i] *= 2;
    for (std::size_t i = count / 2 + 1; i < count; ++i) spectrum[i] = 0;
    designTransform(spectrum, false);
    for (auto& value : spectrum) value = std::exp(value);
    designTransform(spectrum, true);
    std::vector<double> impulse(GraphicEq::kFilterTaps);
    for (std::size_t i = 0; i < impulse.size(); ++i) {
        const double window = 0.5 * (1 + std::cos(kPi * static_cast<double>(i) / impulse.size()));
        impulse[i] = spectrum[i].real() * window;
    }
    return impulse;
}
}

double GraphicEq::targetDb(const std::vector<GraphicBand>& points, double frequency) {
    if (points.empty() || !std::isfinite(frequency) || frequency < 0)
        throw std::invalid_argument("Invalid graphic response request");
    double previous = 0;
    for (const auto& point : points) {
        if (!std::isfinite(point.freq) || point.freq < 20 || point.freq > 20000
            || point.freq <= previous || !std::isfinite(point.gain))
            throw std::invalid_argument("Invalid graphic response points");
        previous = point.freq;
    }
    if (frequency <= points.front().freq) return points.front().gain;
    if (frequency >= points.back().freq) return points.back().gain;
    const auto right = std::upper_bound(points.begin(), points.end(), frequency,
        [](double value, const GraphicBand& point) { return value < point.freq; });
    const auto& left = *(right - 1);
    const double ratio = std::log(frequency / left.freq) / std::log(right->freq / left.freq);
    return left.gain + ratio * (right->gain - left.gain);
}

void GraphicEq::configure(const std::vector<GraphicBand>& points, double rate, unsigned channels) {
    if (!std::isfinite(rate) || rate < 8000 || rate > 384000 || channels < 2 || channels > 8)
        throw std::invalid_argument("Unsupported graphic EQ format");
    Parameters p;
    p.eq.points = points;
    p = clampParameters(std::move(p));
    const auto& normalized = p.eq.points;
    const bool constant = std::all_of(normalized.begin(), normalized.end(),
        [&](const GraphicBand& point) { return point.gain == normalized.front().gain; });
    auto impulse = constant ? std::vector<double>{std::pow(10.0, normalized.front().gain / 20)}
                            : designImpulse(normalized, rate);
    std::vector<Complex> spectra;
    std::vector<Channel> states(channels);
    const auto partitions = constant ? std::size_t(0) : kFilterTaps / kPartitionFrames;
    if (!constant) {
        spectra.resize(partitions * kSpectrumSize);
        std::vector<Complex> block(kTransformSize);
        for (std::size_t partition = 0; partition < partitions; ++partition) {
            std::fill(block.begin(), block.end(), Complex(0, 0));
            for (std::size_t i = 0; i < kPartitionFrames; ++i)
                block[i] = impulse[partition * kPartitionFrames + i];
            designTransform(block, false);
            std::copy_n(block.begin(), kSpectrumSize, spectra.begin() + partition * kSpectrumSize);
        }
        for (auto& state : states) state.history.resize(partitions * kSpectrumSize);
    }
    for (std::size_t bin = 0; bin < twiddles_.size(); ++bin) {
        const double angle = -2 * kPi * static_cast<double>(bin) / kTransformSize;
        twiddles_[bin] = Complex(std::cos(angle), std::sin(angle));
    }
    for (std::size_t index = 0; index < reverse_.size(); ++index) {
        std::size_t value = index, reversed = 0;
        for (std::size_t bit = kTransformSize; bit > 1; bit >>= 1) { reversed = (reversed << 1) | (value & 1); value >>= 1; }
        reverse_[index] = reversed;
    }
    spectra_.swap(spectra); channels_.swap(states); impulse_.swap(impulse);
    partitions_ = partitions; head_ = 0; position_ = 0;
    sampleRate_ = rate; scalar_ = constant ? impulse_[0] : 1;
}

void GraphicEq::transformBlock(std::array<Complex, kTransformSize>& block, bool inverse) const noexcept {
    for (std::size_t index = 0; index < block.size(); ++index)
        if (index < reverse_[index]) std::swap(block[index], block[reverse_[index]]);
    for (std::size_t width = 2; width <= kTransformSize; width *= 2) {
        const auto step = kTransformSize / width;
        for (std::size_t start = 0; start < kTransformSize; start += width) {
            for (std::size_t offset = 0; offset < width / 2; ++offset) {
                const auto factor = inverse ? std::conj(twiddles_[offset * step]) : twiddles_[offset * step];
                const auto even = block[start + offset];
                const auto odd = block[start + offset + width / 2] * factor;
                block[start + offset] = even + odd;
                block[start + offset + width / 2] = even - odd;
            }
        }
    }
    if (inverse) for (auto& value : block) value /= static_cast<double>(kTransformSize);
}

void GraphicEq::completeBlock() noexcept {
    for (auto& channel : channels_) {
        std::fill(channel.work.begin(), channel.work.end(), Complex(0, 0));
        for (std::size_t i = 0; i < kPartitionFrames; ++i) channel.work[i] = channel.input[i];
        transformBlock(channel.work, false);
        std::copy_n(channel.work.begin(), kSpectrumSize, channel.history.begin() + head_ * kSpectrumSize);
        std::fill(channel.accumulator.begin(), channel.accumulator.end(), Complex(0, 0));
        for (std::size_t partition = 0; partition < partitions_; ++partition) {
            const auto historyPosition = (head_ + partitions_ - partition) % partitions_;
            const auto* history = channel.history.data() + historyPosition * kSpectrumSize;
            const auto* filter = spectra_.data() + partition * kSpectrumSize;
            for (std::size_t bin = 0; bin < kSpectrumSize; ++bin)
                channel.accumulator[bin] += history[bin] * filter[bin];
        }
        for (std::size_t bin = 1; bin < kPartitionFrames; ++bin)
            channel.accumulator[kTransformSize - bin] = std::conj(channel.accumulator[bin]);
        transformBlock(channel.accumulator, true);
        for (std::size_t i = 0; i < kPartitionFrames; ++i) {
            channel.output[i] = channel.accumulator[i].real() + channel.overlap[i];
            channel.overlap[i] = channel.accumulator[i + kPartitionFrames].real();
        }
    }
    head_ = (head_ + 1) % partitions_;
}

void GraphicEq::process(float* samples, std::size_t frames) noexcept {
    if (!samples || channels_.empty() || frames > std::numeric_limits<std::size_t>::max() / channels_.size()) return;
    const double limit = std::numeric_limits<float>::max();
    if (spectra_.empty()) {
        if (scalar_ == 1) return;
        for (std::size_t i = 0; i < frames * channels_.size(); ++i) {
            const double input = std::isfinite(samples[i]) ? samples[i] : 0;
            samples[i] = static_cast<float>(std::clamp(input * scalar_, -limit, limit));
        }
        return;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels_.size(); ++channel) {
            auto& state = channels_[channel];
            const auto index = frame * channels_.size() + channel;
            state.input[position_] = std::isfinite(samples[index]) ? samples[index] : 0;
            const double output = state.output[position_];
            samples[index] = static_cast<float>(std::isfinite(output) ? std::clamp(output, -limit, limit) : 0);
        }
        if (++position_ == kPartitionFrames) { completeBlock(); position_ = 0; }
    }
}

double GraphicEq::responseDb(double frequency) const {
    if (!std::isfinite(frequency) || frequency < 0 || sampleRate_ == 0 || frequency >= sampleRate_ / 2)
        throw std::invalid_argument("Invalid graphic response frequency");
    const double angle = -2 * kPi * frequency / sampleRate_;
    const Complex rotation(std::cos(angle), std::sin(angle));
    Complex factor(1, 0), response(0, 0);
    for (const auto value : impulse_) { response += value * factor; factor *= rotation; }
    return 20 * std::log10(std::max(std::abs(response), 1e-30));
}

} // namespace hikari
