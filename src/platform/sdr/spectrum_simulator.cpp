/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "spectrum_simulator.h"

#include <algorithm>
#include <complex>
#include <cstdlib>
#include <cmath>
#include <mutex>

namespace platform::sdr {
namespace {

std::mutex live_spectrum_mutex;
bool live_spectrum_blanked = false;
bool live_spectrum_ready = false;
uint64_t live_spectrum_sequence = 0;
SpectrumFrame live_spectrum_frame{};
SpectrumFrame live_waterfall_frame{};
std::array<float, kSpectrumBinCount> live_spectrum_power{};
constexpr size_t kLiveSpectrumSamples = 1024;
constexpr float kSpectrumDisplayOffsetDb = 157.0f;
std::array<IqSample, kLiveSpectrumSamples> live_iq_buffer{};
size_t live_iq_count = 0;
size_t live_iq_write_index = 0;
size_t live_samples_since_frame = 0;
constexpr size_t kFrameIntervalSamples = 2400; // 50 ms at 48 kHz.

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

void publish_spectrum() noexcept {
    constexpr float kFullScale = 8388608.0f;
    constexpr float kPi = 3.14159265358979323846f;
    std::array<std::complex<float>, kLiveSpectrumSamples> fft{};
    std::complex<float> mean{};
    for (const auto& iq : live_iq_buffer) {
        mean += std::complex<float>(iq.i / kFullScale, iq.q / kFullScale);
    }
    mean /= static_cast<float>(kLiveSpectrumSamples);
    const auto& window = hann_window();
    for (size_t i = 0; i < fft.size(); ++i) {
        const auto& iq = live_iq_buffer[(live_iq_write_index + i) % fft.size()];
        fft[i] = (std::complex<float>(iq.i / kFullScale, iq.q / kFullScale) - mean) * window[i];
    }
    for (size_t i = 1, j = 0; i < fft.size(); ++i) {
        size_t bit = fft.size() >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(fft[i], fft[j]);
    }
    for (size_t length = 2; length <= fft.size(); length <<= 1) {
        const auto step = std::polar(1.0f, -2.0f * kPi / static_cast<float>(length));
        for (size_t start = 0; start < fft.size(); start += length) {
            std::complex<float> phase{1.0f, 0.0f};
            for (size_t j = 0; j < length / 2; ++j) {
                const auto even = fft[start + j];
                const auto odd = fft[start + j + length / 2] * phase;
                fft[start + j] = even + odd;
                fft[start + j + length / 2] = even - odd;
                phase *= step;
            }
        }
    }
    const auto level = [](float power) {
        const float db = 10.0f * std::log10(std::max(power, 1.0e-16f));
        return static_cast<uint8_t>(std::clamp((db + kSpectrumDisplayOffsetDb) * 3.0f,
                                             0.0f, 255.0f));
    };
    for (size_t bin = 0; bin < kSpectrumBinCount; ++bin) {
        float power = 0.0f;
        // Peak-pool FFT bins so a narrow carrier cannot fall between display pixels.
        for (size_t i = bin * fft.size() / kSpectrumBinCount;
             i < (bin + 1) * fft.size() / kSpectrumBinCount; ++i) {
            power = std::max(power, std::norm(fft[(i + fft.size() / 2) % fft.size()])
                                      / static_cast<float>(fft.size() * fft.size()));
        }
        live_spectrum_power[bin] = live_spectrum_ready
            ? live_spectrum_power[bin] * 0.6f + power * 0.4f : power;
        live_spectrum_frame[bin] = level(live_spectrum_power[bin]);
        live_waterfall_frame[bin] = level(power);
    }
    live_spectrum_ready = true;
    ++live_spectrum_sequence;
}

} // namespace

SpectrumFrame make_simulated_spectrum(uint32_t frame_index) noexcept {
    SpectrumFrame spectrum{};
    const size_t first_center = 54 + (frame_index % 27);
    const size_t second_center = 204 - (frame_index % 39);

    for (size_t bin = 0; bin < spectrum.size(); ++bin) {
        const auto noise = static_cast<uint8_t>(12 + ((bin * 73 + frame_index * 29) % 18));
        spectrum[bin] = std::max({noise,
                                  peak_level(bin, first_center, 12, 220),
                                  peak_level(bin, second_center, 21, 184)});
    }

    return spectrum;
}

void LiveSpectrumBuffer::push_iq(const IqSample* samples, size_t sample_count) noexcept {
    if (!samples || sample_count == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(live_spectrum_mutex);
    if (live_spectrum_blanked) return;
    for (size_t sample = 0; sample < sample_count; ++sample) {
        live_iq_buffer[live_iq_write_index] = samples[sample];
        live_iq_write_index = (live_iq_write_index + 1) % kLiveSpectrumSamples;
        live_iq_count = std::min(live_iq_count + 1, kLiveSpectrumSamples);
        ++live_samples_since_frame;
        if (live_iq_count == kLiveSpectrumSamples
            && (!live_spectrum_ready || live_samples_since_frame >= kFrameIntervalSamples)) {
            publish_spectrum();
            live_samples_since_frame = 0;
        }
    }
}

void LiveSpectrumBuffer::set_blanked(bool blanked) noexcept {
    std::lock_guard<std::mutex> lock(live_spectrum_mutex);
    if (live_spectrum_blanked == blanked) return;
    live_spectrum_blanked = blanked;
    live_spectrum_power.fill(0.0f);
    live_spectrum_frame = {};
    live_waterfall_frame = {};
    live_iq_count = 0;
    live_iq_write_index = 0;
    live_samples_since_frame = 0;
    live_spectrum_ready = blanked;
    ++live_spectrum_sequence;
}

bool LiveSpectrumBuffer::snapshot(SpectrumFrame& frame, SpectrumFrame* waterfall,
                                  uint64_t* sequence) noexcept {
    std::lock_guard<std::mutex> lock(live_spectrum_mutex);
    if (!live_spectrum_ready) return false;
    frame = live_spectrum_frame;
    if (waterfall) *waterfall = live_waterfall_frame;
    if (sequence) *sequence = live_spectrum_sequence;
    return true;
}

} // namespace platform::sdr