/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/voice_modulator.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {

// Power of the I/Q signal at +/-tone_hz, measured with a single-bin DFT.
double bin_power(const std::vector<int16_t>& iq, size_t start, double hz) {
    const double kPi = 3.14159265358979;
    double re = 0.0;
    double im = 0.0;
    const size_t count = iq.size() / 2;
    for (size_t n = start; n < count; ++n) {
        const double phase = -2.0 * kPi * hz * static_cast<double>(n) / 48000.0;
        const double i = iq[2 * n];
        const double q = iq[2 * n + 1];
        re += i * std::cos(phase) - q * std::sin(phase);
        im += i * std::sin(phase) + q * std::cos(phase);
    }
    return re * re + im * im;
}

std::vector<int16_t> run(model::RadioMode mode, double tone_hz, int amplitude) {
    const double kPi = 3.14159265358979;
    std::vector<int16_t> mic(24000);
    for (size_t n = 0; n < mic.size(); ++n) {
        mic[n] = static_cast<int16_t>(amplitude * std::sin(2.0 * kPi * tone_hz * static_cast<double>(n) / 48000.0));
    }
    std::vector<int16_t> iq(mic.size() * 2);
    platform::sdr::VoiceModulator modulator;
    modulator.configure(mode, 50);
    modulator.process(mic.data(), mic.size(), iq.data());
    return iq;
}

} // namespace

int main() {
    // A 1 kHz tone lands on one side only. The radio's I/Q is mirrored,
    // so USB sits at -1 kHz in the baseband stream and LSB at +1 kHz.
    const auto usb = run(model::RadioMode::Usb, 1000.0, 3000);
    assert(bin_power(usb, 4000, -1000.0) > 300.0 * bin_power(usb, 4000, 1000.0));
    const auto lsb = run(model::RadioMode::Lsb, 1000.0, 3000);
    assert(bin_power(lsb, 4000, 1000.0) > 300.0 * bin_power(lsb, 4000, -1000.0));

    // Loud audio never exceeds the safe peak level.
    const auto loud = run(model::RadioMode::Usb, 1000.0, 32000);
    for (size_t n = 0; n < loud.size() / 2; ++n) {
        const double magnitude = std::hypot(loud[2 * n], loud[2 * n + 1]);
        assert(magnitude <= platform::sdr::VoiceModulator::kPeakLevel + 2);
    }

    // AM idles at half the peak (the carrier) with no quadrature component.
    const auto am = run(model::RadioMode::Am, 1000.0, 0);
    assert(std::abs(am[2 * 20000] - platform::sdr::VoiceModulator::kPeakLevel / 2) <= 2);
    assert(am[2 * 20000 + 1] == 0);
    return 0;
}
