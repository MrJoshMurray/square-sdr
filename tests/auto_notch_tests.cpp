/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/auto_notch.h"

#include <cassert>
#include <cmath>
#include <random>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979;

double tone_power(const std::vector<float>& samples, size_t start, double hz) {
    double re = 0.0;
    double im = 0.0;
    for (size_t n = start; n < samples.size(); ++n) {
        const double phase = 2.0 * kPi * hz * static_cast<double>(n) / 48000.0;
        re += samples[n] * std::cos(phase);
        im += samples[n] * std::sin(phase);
    }
    return re * re + im * im;
}

} // namespace

int main() {
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.05f);

    // A steady tone buried in noise is removed; the noise passes through.
    std::vector<float> input(48000 * 2);
    for (size_t n = 0; n < input.size(); ++n) {
        input[n] = 0.2f * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n) / 48000.0) + noise(rng);
    }
    auto output = input;
    platform::sdr::AutoNotch notch;
    notch.process(output.data(), output.size());
    assert(tone_power(output, 48000, 1000.0) < tone_power(input, 48000, 1000.0) * 0.01);

    // Noise alone is left essentially untouched.
    std::vector<float> hiss(48000);
    for (auto& sample : hiss) sample = noise(rng);
    auto hiss_out = hiss;
    notch.reset();
    notch.process(hiss_out.data(), hiss_out.size());
    double before = 0.0;
    double after = 0.0;
    for (size_t n = 0; n < hiss.size(); ++n) {
        before += hiss[n] * hiss[n];
        after += hiss_out[n] * hiss_out[n];
    }
    assert(after > before * 0.8 && after < before * 1.2);
    return 0;
}
