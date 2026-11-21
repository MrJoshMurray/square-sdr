/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_model.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace platform::sdr {

// Turns 48 kHz microphone audio into 48 kHz transmit I/Q for USB, LSB and AM.
class VoiceModulator {
public:
    // Peak I/Q magnitude ever produced; matches the level proven safe for the CW carrier.
    static constexpr int kPeakLevel = 0x5A00;

    VoiceModulator();

    void configure(model::RadioMode mode, int mic_gain_percent, int low_cut_hz = 300);
    void reset();
    // Writes interleaved I,Q pairs; `iq` must hold 2 * count values.
    void process(const int16_t* mic, size_t count, int16_t* iq);

private:
    float filter_audio(float sample);
    float filter_hilbert(float sample, float& delayed);

    model::RadioMode mode_{model::RadioMode::Usb};
    int low_cut_hz_{0};
    float gain_{1.0f};
    std::vector<float> bandpass_taps_;
    std::vector<float> hilbert_taps_;
    std::vector<float> audio_history_;
    std::vector<float> hilbert_history_;
    size_t audio_pos_{0};
    size_t hilbert_pos_{0};
    float dc_x_{0.0f};
    float dc_y_{0.0f};
    float envelope_{0.0f};
    size_t ramp_pos_{0};
    bool dc_primed_{false};
};

} // namespace platform::sdr
