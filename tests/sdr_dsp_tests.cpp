/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/receive_demodulator.h"
#include "sdr/spectrum_simulator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include <liquid/liquid.h>

namespace {

constexpr uint32_t kSampleRateHz = 48000;
constexpr size_t kSampleCount = 12000;
constexpr size_t kTransientSamples = 2000;
constexpr float kPi = 3.14159265358979323846f;

int32_t quantize(float sample) {
    const float clipped = std::clamp(sample, -1.0f, 0.99999988f);
    return static_cast<int32_t>(std::lround(clipped * 8388608.0f));
}

float rms(const std::vector<float>& samples) {
    double sum = 0.0;
    size_t count = 0;
    for (size_t index = kTransientSamples; index < samples.size(); ++index) {
        sum += static_cast<double>(samples[index]) * samples[index];
        ++count;
    }
    return static_cast<float>(std::sqrt(sum / count));
}

float tone_amplitude(const std::vector<float>& samples, float frequency_hz) {
    double real = 0.0;
    double imaginary = 0.0;
    size_t count = 0;
    for (size_t index = kTransientSamples; index < samples.size(); ++index) {
        const double phase = 2.0 * kPi * frequency_hz * index / kSampleRateHz;
        real += samples[index] * std::cos(phase);
        imaginary += samples[index] * std::sin(phase);
        ++count;
    }
    return static_cast<float>(2.0 * std::hypot(real, imaginary) / count);
}

void test_am_ssb_mode(platform::sdr::DemodMode mode,
                      liquid_ampmodem_type modulation,
                      int suppressed_carrier) {
    std::vector<platform::sdr::IqSample> iq(kSampleCount);
    std::vector<float> audio(kSampleCount);
    auto modulator = ampmodem_create(0.5f, modulation, suppressed_carrier);
    assert(modulator != nullptr);

    for (size_t index = 0; index < kSampleCount; ++index) {
        const float input = 0.25f * std::sin(2.0f * kPi * 1000.0f * index / kSampleRateHz);
        liquid_float_complex signal{};
        assert(ampmodem_modulate(modulator, input, &signal) == LIQUID_OK);
        iq[index] = {quantize(signal.real), quantize(signal.imag)};
    }
    ampmodem_destroy(modulator);

    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    settings.mode = mode;
    std::string error;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(rms(audio) > 0.01f);
    assert(tone_amplitude(audio, 1000.0f) > 0.03f);
    assert(tone_amplitude(audio, 5000.0f) < tone_amplitude(audio, 1000.0f) * 0.25f);
}

void test_cw_mode() {
    std::vector<platform::sdr::IqSample> iq(kSampleCount, {5000000, 0});
    std::vector<float> audio(kSampleCount);
    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    settings.mode = platform::sdr::DemodMode::Cw;
    settings.cw_pitch_hz = 700;
    std::string error;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(rms(audio) > 0.05f);
    assert(tone_amplitude(audio, 700.0f) > 0.1f);
    assert(tone_amplitude(audio, 1200.0f) < tone_amplitude(audio, 700.0f) * 0.25f);

    settings.audio_bandwidth_hz = 100;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(tone_amplitude(audio, 700.0f) > 0.05f);
    demodulator.reset();
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(tone_amplitude(audio, 700.0f) > 0.05f);
    for (size_t index = 0; index < iq.size(); ++index) {
        const float phase = 2.0f * kPi * 1000.0f * index / kSampleRateHz;
        iq[index] = {quantize(0.5f * std::cos(phase)), quantize(0.5f * std::sin(phase))};
    }
    demodulator.reset();
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(rms(audio) < 0.01f);
    for (size_t index = 0; index < iq.size(); ++index) {
        const float phase = 2.0f * kPi * 150.0f * index / kSampleRateHz;
        iq[index] = {quantize(0.1f * std::cos(phase)), quantize(0.1f * std::sin(phase))};
    }
    demodulator.reset();
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    const float narrow = rms(audio);
    settings.audio_bandwidth_hz = 1200;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(rms(audio) > 0.05f);
    assert(narrow < rms(audio) * 0.1f);
}

void test_am_bandwidth() {
    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    settings.mode = platform::sdr::DemodMode::Am;
    settings.audio_bandwidth_hz = 3000;
    std::string error;
    assert(demodulator.configure(settings, error));
    std::vector<platform::sdr::IqSample> iq(kSampleCount);
    std::vector<float> audio(kSampleCount);
    const auto measure_tone = [&](float frequency) {
        demodulator.reset();
        for (size_t index = 0; index < iq.size(); ++index) {
            const float phase = 2.0f * kPi * frequency * index / kSampleRateHz;
            iq[index] = {quantize(0.1f * std::cos(phase)), quantize(0.1f * std::sin(phase))};
        }
        assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
        return *demodulator.signal_level_dbfs();
    };
    const float in_band = measure_tone(500.0f);
    assert(measure_tone(3000.0f) < in_band - 20.0f);

    const auto measure_modulation = [&](uint32_t bandwidth) {
        settings.audio_bandwidth_hz = bandwidth;
        assert(demodulator.configure(settings, error));
        for (size_t index = 0; index < iq.size(); ++index) {
            const float phase = 2.0f * kPi * 3000.0f * index / kSampleRateHz;
            iq[index] = {quantize(0.3f + 0.05f * std::cos(phase)), 0};
        }
        assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
        return tone_amplitude(audio, 3000.0f);
    };
    const float narrow_audio = measure_modulation(3000);
    const float wide_audio = measure_modulation(8000);
    assert(wide_audio > 0.02f);
    assert(narrow_audio < wide_audio * 0.1f);
}

void test_sideband_selection() {
    std::vector<platform::sdr::IqSample> iq(kSampleCount);
    std::vector<float> audio(kSampleCount);
    auto modulator = ampmodem_create(0.5f, LIQUID_AMPMODEM_LSB, 1);
    assert(modulator);
    for (size_t index = 0; index < iq.size(); ++index) {
        liquid_float_complex signal{};
        const float input = 0.25f * std::sin(2.0f * kPi * 1000.0f * index / kSampleRateHz);
        assert(ampmodem_modulate(modulator, input, &signal) == LIQUID_OK);
        iq[index] = {quantize(signal.real), quantize(signal.imag)};
    }
    ampmodem_destroy(modulator);
    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    std::string error;
    settings.mode = platform::sdr::DemodMode::Usb;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    const float selected = tone_amplitude(audio, 1000.0f);
    settings.mode = platform::sdr::DemodMode::Lsb;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    assert(selected > 0.03f);
    assert(tone_amplitude(audio, 1000.0f) < selected * 0.1f);

    modulator = ampmodem_create(0.5f, LIQUID_AMPMODEM_LSB, 1);
    assert(modulator);
    for (size_t index = 0; index < iq.size(); ++index) {
        liquid_float_complex signal{};
        const float input = 0.25f * std::sin(2.0f * kPi * 2400.0f * index / kSampleRateHz);
        assert(ampmodem_modulate(modulator, input, &signal) == LIQUID_OK);
        iq[index] = {quantize(signal.real), quantize(signal.imag)};
    }
    ampmodem_destroy(modulator);
    settings.mode = platform::sdr::DemodMode::Usb;
    settings.audio_bandwidth_hz = 1800;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    const float narrow = tone_amplitude(audio, 2400.0f);
    settings.audio_bandwidth_hz = 3000;
    assert(demodulator.configure(settings, error));
    assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
    const float wide = tone_amplitude(audio, 2400.0f);
    assert(wide > 0.03f);
    assert(narrow < wide * 0.1f);
}

void test_live_spectrum_tones() {
    constexpr size_t sample_count = 1024;
    std::vector<platform::sdr::IqSample> iq(sample_count);
    platform::sdr::SpectrumFrame frame{};
    assert(!platform::sdr::LiveSpectrumBuffer::snapshot(frame));

    for (const int direction : {1, -1}) {
        for (size_t sample = 0; sample < iq.size(); ++sample) {
            const float phase = 2.0f * kPi * direction * 0.25f * sample;
            iq[sample] = {quantize(0.000012f * std::cos(phase)),
                          quantize(0.000012f * std::sin(phase))};
        }
        platform::sdr::LiveSpectrumBuffer::push_iq(iq.data(), iq.size());
        for (int iteration = 0; iteration < 40; ++iteration) {
            assert(platform::sdr::LiveSpectrumBuffer::snapshot(frame));
        }
        const auto peak = std::max_element(frame.begin(), frame.end()) - frame.begin();
        assert(peak == (direction > 0 ? 72 : 24));
        assert(frame[peak] > 0);
    }
}

void test_signal_level() {
    platform::sdr::ReceiveDemodulator demodulator;
    platform::sdr::DemodulatorSettings settings;
    settings.mode = platform::sdr::DemodMode::Usb;
    std::string error;
    assert(demodulator.configure(settings, error));
    assert(!demodulator.signal_level_dbfs());
    std::vector<platform::sdr::IqSample> iq(kSampleCount);
    std::vector<float> audio(kSampleCount);
    const auto measure_tone = [&](float frequency) {
        demodulator.reset();
        for (size_t index = 0; index < iq.size(); ++index) {
            const float phase = 2.0f * kPi * frequency * index / kSampleRateHz;
            iq[index] = {quantize(0.1f * std::cos(phase)), quantize(0.1f * std::sin(phase))};
        }
        assert(demodulator.process(iq.data(), iq.size(), audio.data(), audio.size(), error));
        return *demodulator.signal_level_dbfs();
    };
    const float in_band = measure_tone(-1400.0f);
    assert(in_band > -22.0f && in_band < -18.0f);
    assert(measure_tone(6000.0f) < in_band - 30.0f);
    assert(measure_tone(-6000.0f) < in_band - 30.0f);
    assert(measure_tone(1400.0f) < in_band - 20.0f);
    demodulator.reset();
    assert(!demodulator.signal_level_dbfs());
    settings.mode = platform::sdr::DemodMode::Lsb;
    assert(demodulator.configure(settings, error));
    assert(measure_tone(1400.0f) > -22.0f);
    assert(measure_tone(-1400.0f) < -40.0f);
    settings.mode = platform::sdr::DemodMode::Am;
    assert(demodulator.configure(settings, error));
    assert(measure_tone(0.0f) > -22.0f);
}

} // namespace

int main() {
    test_am_ssb_mode(platform::sdr::DemodMode::Usb, LIQUID_AMPMODEM_LSB, 1);
    test_am_ssb_mode(platform::sdr::DemodMode::Lsb, LIQUID_AMPMODEM_USB, 1);
    test_am_ssb_mode(platform::sdr::DemodMode::Am, LIQUID_AMPMODEM_DSB, 0);
    test_cw_mode();
    test_am_bandwidth();
    test_sideband_selection();
    test_live_spectrum_tones();
    test_signal_level();
}