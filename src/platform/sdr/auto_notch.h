/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>

namespace platform::sdr {

// Adaptive notch for receive audio: an NLMS predictor learns steady tones (carriers, heterodynes)
// from a delayed copy of the audio and subtracts them, leaving speech and noise largely alone.
class AutoNotch {
public:
    static constexpr size_t kTaps = 64;
    static constexpr size_t kDelay = 48;

    AutoNotch();
    void reset();
    void process(float* samples, size_t count);

private:
    static constexpr size_t kHistory = kTaps + kDelay;
    std::array<float, kTaps> weights_{};
    std::array<float, 2 * kHistory> history_{};
    size_t pos_{0};
};

} // namespace platform::sdr
