// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace hikari {
// In-place radix-2 transform with twiddles exp(-2*pi*i*k/n); tables are built once by the constructor.
template <class T>
class Fft {
public:
    explicit Fft(std::size_t size) : size_(size), order_(size), twiddles_(size / 2) {
        if (size < 2 || (size & (size - 1)) != 0) throw std::invalid_argument("FFT size must be a power of two");
        constexpr double pi = 3.141592653589793238462643383279502884;
        for (std::size_t k = 0; k < size / 2; ++k) {
            const double angle = -2 * pi * static_cast<double>(k) / static_cast<double>(size);
            twiddles_[k] = {static_cast<T>(std::cos(angle)), static_cast<T>(std::sin(angle))};
        }
        std::size_t bits = 0;
        while ((std::size_t(1) << bits) < size) ++bits;
        for (std::size_t index = 0; index < size; ++index) {
            std::size_t reversed = 0;
            for (std::size_t bit = 0; bit < bits; ++bit) if (index & (std::size_t(1) << bit)) reversed |= std::size_t(1) << (bits - 1 - bit);
            order_[index] = reversed;
        }
    }
    std::size_t size() const noexcept { return size_; }
    void transform(std::complex<T>* data) const noexcept {
        for (std::size_t index = 0; index < size_; ++index) if (order_[index] > index) std::swap(data[index], data[order_[index]]);
        for (std::size_t length = 2; length <= size_; length <<= 1) {
            const std::size_t half = length / 2, stride = size_ / length;
            for (std::size_t start = 0; start < size_; start += length) {
                for (std::size_t k = 0; k < half; ++k) {
                    const T wr = twiddles_[k * stride].real(), wi = twiddles_[k * stride].imag();
                    auto& low = data[start + k];
                    auto& high = data[start + k + half];
                    const T re = high.real() * wr - high.imag() * wi, im = high.real() * wi + high.imag() * wr;
                    high = {low.real() - re, low.imag() - im};
                    low = {low.real() + re, low.imag() + im};
                }
            }
        }
    }
private:
    std::size_t size_;
    std::vector<std::size_t> order_;
    std::vector<std::complex<T>> twiddles_;
};
} // namespace hikari
