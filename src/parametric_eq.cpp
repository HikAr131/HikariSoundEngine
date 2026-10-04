// SPDX-License-Identifier: AGPL-3.0-or-later
#include "parametric_eq.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace hikari {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
void validateFormat(double rate, unsigned channels) {
    if (!std::isfinite(rate) || rate < 8000 || rate > 384000
        || channels < 2 || channels > 8) throw std::invalid_argument("Unsupported audio format");
}
}

ParametricEq::Coefficients ParametricEq::design(const Filter& f, double rate) {
    const double freq = std::clamp(f.freq, 20.0, rate / 2 - 1);
    const double q = std::max(f.q, 0.0001);
    const double a = std::pow(10.0, f.gain / 40);
    const double omega = 2 * kPi * freq / rate;
    const double c = std::cos(omega);
    const double alpha = std::sin(omega) / (2 * q);
    double b0, b1, b2, a0, a1, a2;
    if (f.type == "LS" || f.type == "LSC") {
        const double term = 2 * std::sqrt(a) * alpha;
        b0 = a * (a + 1 - (a - 1) * c + term);
        b1 = 2 * a * (a - 1 - (a + 1) * c);
        b2 = a * (a + 1 - (a - 1) * c - term);
        a0 = a + 1 + (a - 1) * c + term;
        a1 = -2 * (a - 1 + (a + 1) * c);
        a2 = a + 1 + (a - 1) * c - term;
    } else if (f.type == "HS" || f.type == "HSC") {
        const double term = 2 * std::sqrt(a) * alpha;
        b0 = a * (a + 1 + (a - 1) * c + term);
        b1 = -2 * a * (a - 1 + (a + 1) * c);
        b2 = a * (a + 1 + (a - 1) * c - term);
        a0 = a + 1 - (a - 1) * c + term;
        a1 = 2 * (a - 1 - (a + 1) * c);
        a2 = a + 1 - (a - 1) * c - term;
    } else if (f.type == "LP" || f.type == "LPQ") {
        b0 = (1 - c) / 2; b1 = 1 - c; b2 = b0;
        a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
    } else if (f.type == "HP" || f.type == "HPQ") {
        b0 = (1 + c) / 2; b1 = -(1 + c); b2 = b0;
        a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
    } else {
        // BP, NO, AP, LSQ and HSQ intentionally preserve the existing PK preview approximation.
        b0 = 1 + alpha * a; b1 = -2 * c; b2 = 1 - alpha * a;
        a0 = 1 + alpha / a; a1 = -2 * c; a2 = 1 - alpha / a;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

void ParametricEq::configure(const Parameters& parameters, double rate, unsigned channels) {
    validateFormat(rate, channels);
    const auto p = clampParameters(parameters);
    std::vector<Section> sections;
    if (!p.bypass && p.eq.mode == "parametric") {
        sections.reserve(p.eq.filters.size() + 1);
        for (const auto& filter : p.eq.filters) {
            if (filter.enabled) sections.push_back({design(filter, rate), {}});
        }
    }
    if (!p.bypass && p.effects.treble != 0) {
        Filter treble;
        treble.type = "HSC"; treble.freq = 6000; treble.q = 0.7;
        treble.gain = p.effects.treble * 8 / 100;
        sections.push_back({design(treble, rate), {}});
    }
    GraphicEq graphic;
    graphic.configure(!p.bypass && p.eq.mode == "graphic" ? p.eq.points : std::vector<GraphicBand>{{1000, 0}}, rate, channels);
    sections_.swap(sections);
    graphic_ = std::move(graphic);
    channels_ = channels;
    preamp_ = p.eq.mode == "off" ? 1 : std::pow(10.0, p.eq.preamp / 20);
    bypass_ = p.bypass;
}

void ParametricEq::process(float* samples, std::size_t frames) noexcept {
    if (!samples || bypass_ || channels_ == 0) return;
    if (frames > std::numeric_limits<std::size_t>::max() / channels_) return;
    const double limit = std::numeric_limits<float>::max();
    if (preamp_ != 1) {
        for (std::size_t i = 0; i < frames * channels_; ++i) {
            const double input = std::isfinite(samples[i]) ? samples[i] : 0;
            samples[i] = static_cast<float>(std::clamp(input * preamp_, -limit, limit));
        }
    }
    graphic_.process(samples, frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < channels_; ++channel) {
            const auto index = frame * channels_ + channel;
            double x = std::isfinite(samples[index]) ? samples[index] : 0;
            for (auto& section : sections_) {
                const auto& c = section.coefficients;
                auto& s = section.channels[channel];
                const double y = c.b0 * x + s.z1;
                s.z1 = c.b1 * x - c.a1 * y + s.z2;
                s.z2 = c.b2 * x - c.a2 * y;
                if (!std::isfinite(y) || !std::isfinite(s.z1) || !std::isfinite(s.z2)) {
                    s = {}; x = 0;
                } else {
                    if (std::abs(s.z1) < 1e-30) s.z1 = 0;
                    if (std::abs(s.z2) < 1e-30) s.z2 = 0;
                    x = y;
                }
            }
            samples[index] = static_cast<float>(std::clamp(x, -limit, limit));
        }
    }
}

double ParametricEq::responseDb(const Filter& filter, double frequency, double rate) {
    validateFormat(rate, 2);
    if (!std::isfinite(frequency) || frequency <= 0 || frequency >= rate / 2)
        throw std::invalid_argument("Invalid response frequency");
    Parameters p;
    p.eq.filters.push_back(filter);
    p = clampParameters(p);
    if (!p.eq.filters.front().enabled) return 0;
    const auto c = design(p.eq.filters.front(), rate);
    const double omega = 2 * kPi * frequency / rate;
    const double cosine = std::cos(omega), sine = std::sin(omega);
    const double cosine2 = std::cos(2 * omega), sine2 = std::sin(2 * omega);
    const double numerator = std::hypot(c.b0 + c.b1 * cosine + c.b2 * cosine2,
        -(c.b1 * sine + c.b2 * sine2));
    const double denominator = std::hypot(1 + c.a1 * cosine + c.a2 * cosine2,
        -(c.a1 * sine + c.a2 * sine2));
    return 20 * std::log10(numerator / (denominator == 0 ? 1e-12 : denominator));
}

} // namespace hikari
