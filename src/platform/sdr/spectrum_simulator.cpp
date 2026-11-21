/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "spectrum_simulator.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <mutex>

namespace platform::sdr {
namespace {

std::mutex live_spectrum_mutex;
SpectrumFrame live_spectrum_frame{};
std::array<float, kSpectrumBinCount> live_spectrum_power{};
constexpr size_t kLiveSpectrumSamples = 1024;
constexpr float kSpectrumDisplayOffsetDb = 157.0f;
std::array<IqSample, kLiveSpectrumSamples> live_iq_buffer{};
size_t live_iq_count = 0;
size_t live_iq_write_index = 0;

const std::array<float, kLiveSpectrumSamples>& hann_window() noexcept {
    static const auto window = [] {
        std::array<float, kLiveSpectrumSamples> values{};
        constexpr float kPi = 3.14159265358979323846f;
        for (size_t sample = 0; sample < kLiveSpectrumSamples; ++sample) {
            values[sample] = 0.5f - 0.5f * std::cos(
                2.0f * kPi * static_cast<float>(sample)
                / static_cast<float>(kLiveSpectrumSamples));
        }
        return values;
    }();
    return window;
}

uint8_t peak_level(size_t bin, size_t center, size_t radius, uint8_t top) noexcept {
    const auto distance = static_cast<size_t>(std::abs(static_cast<int>(bin) - static_cast<int>(center)));
    if (distance > radius) {
        return 0;
    }
    return static_cast<uint8_t>(top - (top / (radius + 1)) * distance);
}

} // namespace

SpectrumFrame make_simulated_spectrum(uint32_t frame_index) noexcept {
    SpectrumFrame spectrum{};
    const size_t first_center = 18 + (frame_index % 9);
    const size_t second_center = 68 - (frame_index % 13);

    for (size_t bin = 0; bin < spectrum.size(); ++bin) {
        const auto noise = static_cast<uint8_t>(12 + ((bin * 73 + frame_index * 29) % 18));
        spectrum[bin] = std::max({noise,
                                  peak_level(bin, first_center, 4, 220),
                                  peak_level(bin, second_center, 7, 184)});
    }

    return spectrum;
}

void LiveSpectrumBuffer::push_iq(const IqSample* samples, size_t sample_count) noexcept {
    if (!samples || sample_count == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(live_spectrum_mutex);
    for (size_t sample = 0; sample < sample_count; ++sample) {
        live_iq_buffer[live_iq_write_index] = samples[sample];
        live_iq_write_index = (live_iq_write_index + 1) % kLiveSpectrumSamples;
        live_iq_count = std::min(live_iq_count + 1, kLiveSpectrumSamples);
    }
}

bool LiveSpectrumBuffer::snapshot(SpectrumFrame& frame) noexcept {
    std::array<IqSample, kLiveSpectrumSamples> samples{};
    {
        std::lock_guard<std::mutex> lock(live_spectrum_mutex);
        if (live_iq_count < kLiveSpectrumSamples) {
            return false;
        }

        const size_t first_sample = live_iq_write_index;
        for (size_t sample = 0; sample < kLiveSpectrumSamples; ++sample) {
            samples[sample] = live_iq_buffer[
                (first_sample + sample) % kLiveSpectrumSamples];
        }
    }

    constexpr float kFullScale = 8388608.0f;
    const auto& window = hann_window();
    std::array<float, kSpectrumBinCount> power{};
    float mean_i = 0.0f;
    float mean_q = 0.0f;
    for (size_t sample = 0; sample < kLiveSpectrumSamples; ++sample) {
        const auto& iq = samples[sample];
        mean_i += static_cast<float>(iq.i) / kFullScale;
        mean_q += static_cast<float>(iq.q) / kFullScale;
    }
    mean_i /= static_cast<float>(kLiveSpectrumSamples);
    mean_q /= static_cast<float>(kLiveSpectrumSamples);

    for (size_t bin = 0; bin < kSpectrumBinCount; ++bin) {
        const float normalized_bin = static_cast<float>(bin)
                                   / static_cast<float>(kSpectrumBinCount);
        const float centered_bin = normalized_bin - 0.5f;
        float real_sum = 0.0f;
        float imag_sum = 0.0f;
        const float phase_step = -2.0f * 3.14159265358979323846f
                               * centered_bin;
        float phase_cos = 1.0f;
        float phase_sin = 0.0f;
        const float step_cos = std::cos(phase_step);
        const float step_sin = std::sin(phase_step);
        for (size_t sample = 0; sample < kLiveSpectrumSamples; ++sample) {
            const auto& iq = samples[sample];
            const float i = static_cast<float>(iq.i) / kFullScale - mean_i;
            const float q = static_cast<float>(iq.q) / kFullScale - mean_q;
            const float weighted_i = window[sample] * i;
            const float weighted_q = window[sample] * q;
            real_sum += weighted_i * phase_cos - weighted_q * phase_sin;
            imag_sum += weighted_i * phase_sin + weighted_q * phase_cos;
            const float next_cos = phase_cos * step_cos - phase_sin * step_sin;
            phase_sin = phase_sin * step_cos + phase_cos * step_sin;
            phase_cos = next_cos;
        }
        power[bin] = (real_sum * real_sum + imag_sum * imag_sum)
                   / static_cast<float>(kLiveSpectrumSamples * kLiveSpectrumSamples);
    }

    SpectrumFrame next{};
    for (size_t bin = 0; bin < next.size(); ++bin) {
        live_spectrum_power[bin] = live_spectrum_power[bin] * 0.85f + power[bin] * 0.15f;
        const float decibels = 10.0f * std::log10(std::max(live_spectrum_power[bin], 1.0e-16f));
        const float scaled = std::clamp((decibels + kSpectrumDisplayOffsetDb) * 3.0f,
                        0.0f,
                        255.0f);
        next[bin] = static_cast<uint8_t>(scaled);
    }
    live_spectrum_frame = next;
    frame = live_spectrum_frame;
    return true;
}

} // namespace platform::sdr