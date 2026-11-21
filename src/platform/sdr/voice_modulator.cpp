/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "voice_modulator.h"

#include <algorithm>
#include <cmath>

namespace platform::sdr {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr size_t kBandpassLength = 201;
constexpr size_t kHilbertLength = 301;

double blackman(size_t n, size_t length) {
    const double x = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(length - 1);
    return 0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x);
}

std::vector<float> make_bandpass(double low_hz, double high_hz) {
    std::vector<float> taps(kBandpassLength);
    const double centre = static_cast<double>(kBandpassLength - 1) / 2.0;
    const double wl = 2.0 * kPi * low_hz / kSampleRate;
    const double wh = 2.0 * kPi * high_hz / kSampleRate;
    for (size_t n = 0; n < kBandpassLength; ++n) {
        const double k = static_cast<double>(n) - centre;
        double ideal = (wh - wl) / kPi;
        if (k != 0.0) ideal = (std::sin(wh * k) - std::sin(wl * k)) / (kPi * k);
        taps[n] = static_cast<float>(ideal * blackman(n, kBandpassLength));
    }
    return taps;
}

std::vector<float> make_hilbert() {
    std::vector<float> taps(kHilbertLength, 0.0f);
    const int centre = static_cast<int>((kHilbertLength - 1) / 2);
    for (size_t n = 0; n < kHilbertLength; ++n) {
        const int k = static_cast<int>(n) - centre;
        if (k % 2 == 0) continue;
        taps[n] = static_cast<float>(2.0 / (kPi * k) * blackman(n, kHilbertLength));
    }
    return taps;
}

} // namespace

VoiceModulator::VoiceModulator()
    : hilbert_taps_(make_hilbert()),
      audio_history_(kBandpassLength, 0.0f),
      hilbert_history_(kHilbertLength, 0.0f) {
    configure(model::RadioMode::Usb, 50);
}

void VoiceModulator::configure(model::RadioMode mode, int mic_gain_percent, int low_cut_hz) {
    // AM keeps its fixed wider band; the low cut only shapes the SSB passband.
    const int low_cut = mode == model::RadioMode::Am ? 100 : std::clamp(low_cut_hz, 100, 300);
    if (bandpass_taps_.empty() || mode != mode_ || low_cut != low_cut_hz_) {
        // AM carries a wider audio band than SSB.
        bandpass_taps_ = mode == model::RadioMode::Am ? make_bandpass(100.0, 3500.0)
                                                      : make_bandpass(low_cut, 2700.0);
    }
    mode_ = mode;
    low_cut_hz_ = low_cut;
    // Trim applied after the AGC: -30 dB to +30 dB across the 0-100% setting.
    const double db = (std::clamp(mic_gain_percent, 0, 100) - 50) * 0.6;
    gain_ = static_cast<float>(std::pow(10.0, db / 20.0));
}

void VoiceModulator::reset() {
    std::fill(audio_history_.begin(), audio_history_.end(), 0.0f);
    std::fill(hilbert_history_.begin(), hilbert_history_.end(), 0.0f);
    audio_pos_ = 0;
    hilbert_pos_ = 0;
    dc_x_ = 0.0f;
    dc_y_ = 0.0f;
    envelope_ = 0.0f;
    ramp_pos_ = 0;
    dc_primed_ = false;
}

float VoiceModulator::filter_audio(float sample) {
    audio_history_[audio_pos_] = sample;
    float sum = 0.0f;
    size_t index = audio_pos_;
    for (size_t k = 0; k < kBandpassLength; ++k) {
        sum += bandpass_taps_[k] * audio_history_[index];
        index = index == 0 ? kBandpassLength - 1 : index - 1;
    }
    audio_pos_ = (audio_pos_ + 1) % kBandpassLength;
    return sum;
}

float VoiceModulator::filter_hilbert(float sample, float& delayed) {
    hilbert_history_[hilbert_pos_] = sample;
    float sum = 0.0f;
    size_t index = hilbert_pos_;
    for (size_t k = 0; k < kHilbertLength; ++k) {
        // Even taps are zero, so only odd taps contribute.
        if (k % 2 == 1) sum += hilbert_taps_[k] * hilbert_history_[index];
        index = index == 0 ? kHilbertLength - 1 : index - 1;
    }
    const size_t centre = (kHilbertLength - 1) / 2;
    delayed = hilbert_history_[(hilbert_pos_ + kHilbertLength - centre) % kHilbertLength];
    hilbert_pos_ = (hilbert_pos_ + 1) % kHilbertLength;
    return sum;
}

void VoiceModulator::process(const int16_t* mic, size_t count, int16_t* iq) {
    constexpr float kPeak = static_cast<float>(kPeakLevel);
    for (size_t n = 0; n < count; ++n) {
        // DC blocker, then gain and a hard clip that acts as a simple speech processor;
        // the band-pass that follows removes the clipping harmonics.
        const float x = static_cast<float>(mic[n]) / 32768.0f;
        if (!dc_primed_) {
            dc_x_ = x;
            dc_primed_ = true;
        }
        const float blocked = x - dc_x_ + 0.995f * dc_y_;
        dc_x_ = x;
        dc_y_ = blocked;
        // Fast-attack, slow-release AGC levels quiet on-board microphones up to a useful
        // speech level; the floor stops it from boosting background noise without limit.
        constexpr float kAgcTarget = 0.8f;
        constexpr float kAgcFloor = 0.02f;
        constexpr float kAgcRelease = 0.99997f;
        envelope_ = std::max(std::fabs(blocked), envelope_ * kAgcRelease);
        const float agc = kAgcTarget / std::max(envelope_, kAgcFloor);
        const float clipped = std::clamp(blocked * agc * gain_, -1.0f, 1.0f);
        const float audio = filter_audio(clipped);

        float i = 0.0f;
        float q = 0.0f;
        if (mode_ == model::RadioMode::Am) {
            const float modulation = std::clamp(audio, -1.0f, 1.0f);
            i = kPeak * 0.5f * (1.0f + modulation);
        } else {
            float delayed = 0.0f;
            q = filter_hilbert(audio, delayed);
            i = delayed;
            // The radio's transmit I/Q is mirrored relative to the textbook analytic signal,
            // so USB needs the inverted quadrature (confirmed on hardware).
            if (mode_ == model::RadioMode::Usb) q = -q;
            i *= kPeak * 0.9f;
            q *= kPeak * 0.9f;
            // Limit the envelope so peaks never exceed the proven-safe level.
            const float magnitude = std::sqrt(i * i + q * q);
            if (magnitude > kPeak) {
                i *= kPeak / magnitude;
                q *= kPeak / magnitude;
            }
        }
        // Fade in over ~20 ms at key-up so the carrier or noise burst does not click.
        constexpr size_t kRampSamples = 960;
        if (ramp_pos_ < kRampSamples) {
            const float t = static_cast<float>(ramp_pos_++) / static_cast<float>(kRampSamples);
            const float fade = t * t * (3.0f - 2.0f * t);
            i *= fade;
            q *= fade;
        }
        iq[2 * n] = static_cast<int16_t>(std::lround(std::clamp(i, -kPeak, kPeak)));
        iq[2 * n + 1] = static_cast<int16_t>(std::lround(std::clamp(q, -kPeak, kPeak)));
    }
}

} // namespace platform::sdr
