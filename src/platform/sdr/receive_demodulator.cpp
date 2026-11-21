/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "receive_demodulator.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <liquid/liquid.h>

namespace platform::sdr {
namespace {

constexpr uint32_t kAmBandwidthHz = 4500;
constexpr uint32_t kSsbBandwidthHz = 2800;
constexpr uint32_t kCwBandwidthHz = 900;
constexpr unsigned int kAudioFilterTaps = 129;
constexpr float kDcBlockerAlpha = 0.001f;
constexpr float kAmModulationIndex = 0.5f;

uint32_t default_bandwidth(DemodMode mode) {
    switch (mode) {
    case DemodMode::Am:
        return kAmBandwidthHz;
    case DemodMode::Cw:
        return kCwBandwidthHz;
    case DemodMode::Usb:
    case DemodMode::Lsb:
        return kSsbBandwidthHz;
    }
    return kSsbBandwidthHz;
}

} // namespace

struct ReceiveDemodulator::Impl {
    ampmodem amplitude_demodulator{nullptr};
    nco_crcf cw_oscillator{nullptr};
    iirfilt_crcf cw_filter{nullptr};
    float cw_frequency{0.0f};
    firfilt_rrrf audio_filter{nullptr};
    iirfilt_rrrf dc_blocker{nullptr};
    firfilt_crcf signal_filter{nullptr};
    nco_crcf signal_mixer{nullptr};
    float signal_mixer_frequency{0.0f};
    float signal_power{0.0f};
    bool has_signal_level{false};
    DemodMode mode{DemodMode::Usb};

    ~Impl() {
        if (amplitude_demodulator) {
            ampmodem_destroy(amplitude_demodulator);
        }
        if (cw_oscillator) {
            nco_crcf_destroy(cw_oscillator);
        }
        if (cw_filter) iirfilt_crcf_destroy(cw_filter);
        if (audio_filter) {
            firfilt_rrrf_destroy(audio_filter);
        }
        if (dc_blocker) {
            iirfilt_rrrf_destroy(dc_blocker);
        }
        if (signal_filter) firfilt_crcf_destroy(signal_filter);
        if (signal_mixer) nco_crcf_destroy(signal_mixer);
    }
};

ReceiveDemodulator::ReceiveDemodulator() = default;
ReceiveDemodulator::~ReceiveDemodulator() = default;

bool ReceiveDemodulator::configure(const DemodulatorSettings& settings, std::string& error) {
    error.clear();
    if (settings.sample_rate_hz < 12000) {
        error = "Sample rate is too low for voice demodulation";
        return false;
    }
    if (settings.mode == DemodMode::Cw &&
        (settings.cw_pitch_hz == 0 || settings.cw_pitch_hz >= settings.sample_rate_hz / 2)) {
        error = "CW pitch must be below the Nyquist frequency";
        return false;
    }

    const uint32_t bandwidth_hz = settings.audio_bandwidth_hz == 0
                                ? default_bandwidth(settings.mode)
                                : settings.audio_bandwidth_hz;
    if (bandwidth_hz == 0 || bandwidth_hz >= settings.sample_rate_hz / 2) {
        error = "Audio bandwidth must be below the Nyquist frequency";
        return false;
    }

    auto next = std::make_unique<Impl>();
    next->mode = settings.mode;
    switch (settings.mode) {
    case DemodMode::Usb:
        next->amplitude_demodulator = ampmodem_create(kAmModulationIndex,
                                                      LIQUID_AMPMODEM_LSB,
                                                      1);
        break;
    case DemodMode::Lsb:
        next->amplitude_demodulator = ampmodem_create(kAmModulationIndex,
                                                      LIQUID_AMPMODEM_USB,
                                                      1);
        break;
    case DemodMode::Am:
        next->amplitude_demodulator = ampmodem_create(kAmModulationIndex,
                                                      LIQUID_AMPMODEM_DSB,
                                                      0);
        break;
    case DemodMode::Cw:
        next->cw_oscillator = nco_crcf_create(LIQUID_NCO);
        if (next->cw_oscillator) {
            next->cw_frequency = 2.0f * static_cast<float>(M_PI)
                                           * static_cast<float>(settings.cw_pitch_hz)
                                           / static_cast<float>(settings.sample_rate_hz);
            nco_crcf_set_frequency(next->cw_oscillator, next->cw_frequency);
        }
        break;
    }

    const float cutoff = static_cast<float>(bandwidth_hz)
                       / static_cast<float>(settings.sample_rate_hz);
    const float audio_cutoff = settings.mode == DemodMode::Cw
                             ? std::min(0.45f, (static_cast<float>(settings.cw_pitch_hz)
                                               + static_cast<float>(bandwidth_hz) * 0.5f + 600.0f)
                                              / static_cast<float>(settings.sample_rate_hz))
                             : settings.mode == DemodMode::Am ? cutoff * 0.5f : cutoff;
    next->audio_filter = firfilt_rrrf_create_kaiser(kAudioFilterTaps, audio_cutoff, 60.0f, 0.0f);
    if (settings.mode == DemodMode::Cw) next->cw_filter = iirfilt_crcf_create_lowpass(4, cutoff * 0.5f);
    next->dc_blocker = iirfilt_rrrf_create_dc_blocker(kDcBlockerAlpha);
    const bool sideband = settings.mode == DemodMode::Usb || settings.mode == DemodMode::Lsb;
    const float signal_cutoff = cutoff * 0.5f;
    next->signal_filter = firfilt_crcf_create_kaiser(kAudioFilterTaps, signal_cutoff, 60.0f, 0.0f);
    std::array<float, kAudioFilterTaps> signal_coefficients{};
    liquid_firdes_kaiser(kAudioFilterTaps, signal_cutoff, 60.0f, 0.0f, signal_coefficients.data());
    float signal_gain = 0.0f;
    for (const float coefficient : signal_coefficients) signal_gain += coefficient;
    if (next->signal_filter && signal_gain != 0.0f) {
        firfilt_crcf_set_scale(next->signal_filter, 1.0f / signal_gain);
    }
    next->signal_mixer = nco_crcf_create(LIQUID_NCO);
    if (next->signal_mixer && sideband) {
        const float direction = settings.mode == DemodMode::Usb ? -1.0f : 1.0f;
        next->signal_mixer_frequency = direction * static_cast<float>(M_PI) * cutoff;
        nco_crcf_set_frequency(next->signal_mixer, next->signal_mixer_frequency);
    }

    if ((settings.mode == DemodMode::Cw && (!next->cw_oscillator || !next->cw_filter))
        || (settings.mode != DemodMode::Cw && !next->amplitude_demodulator)
        || !next->audio_filter || !next->dc_blocker || !next->signal_filter || !next->signal_mixer) {
        error = "Liquid-DSP could not allocate the receiver filters";
        return false;
    }

    impl_ = std::move(next);
    return true;
}

bool ReceiveDemodulator::process(const IqSample* input,
                                size_t sample_count,
                                float* mono_output,
                                size_t output_capacity,
                                std::string& error) {
    error.clear();
    if (!impl_) {
        error = "Demodulator is not configured";
        return false;
    }
    if (sample_count > output_capacity) {
        error = "Audio output buffer is too small";
        return false;
    }
    if (sample_count != 0 && (!input || !mono_output)) {
        error = "I/Q or audio buffer is null";
        return false;
    }

    for (size_t index = 0; index < sample_count; ++index) {
        liquid_float_complex iq{};
        iq.real = static_cast<float>(input[index].i) / 8388608.0f;
        iq.imag = static_cast<float>(input[index].q) / 8388608.0f;
        liquid_float_complex shifted_signal{};
        liquid_float_complex filtered_signal{};
        if (impl_->mode == DemodMode::Cw) {
            iirfilt_crcf_execute(impl_->cw_filter, iq, &filtered_signal);
        } else {
            nco_crcf_mix_down(impl_->signal_mixer, iq, &shifted_signal);
            nco_crcf_step(impl_->signal_mixer);
            firfilt_crcf_push(impl_->signal_filter, shifted_signal);
            firfilt_crcf_execute(impl_->signal_filter, &filtered_signal);
        }
        const float power = filtered_signal.real * filtered_signal.real
                  + filtered_signal.imag * filtered_signal.imag;
        impl_->signal_power += 0.0005f * (power - impl_->signal_power);
        impl_->has_signal_level = true;
        float demodulated = 0.0f;
        if (impl_->mode == DemodMode::Cw) {
            liquid_float_complex shifted{};
            nco_crcf_mix_down(impl_->cw_oscillator, filtered_signal, &shifted);
            nco_crcf_step(impl_->cw_oscillator);
            demodulated = shifted.real;
        }
        else if (ampmodem_demodulate(impl_->amplitude_demodulator,
                         impl_->mode == DemodMode::Am ? filtered_signal : iq,
                                     &demodulated) != LIQUID_OK) {
            error = "Liquid-DSP failed to demodulate an I/Q sample";
            return false;
        }

        float dc_removed = 0.0f;
        float filtered = 0.0f;
        iirfilt_rrrf_execute(impl_->dc_blocker, demodulated, &dc_removed);
        firfilt_rrrf_execute_one(impl_->audio_filter, dc_removed, &filtered);
        mono_output[index] = std::clamp(filtered, -1.0f, 1.0f);
    }

    return true;
}

std::optional<float> ReceiveDemodulator::signal_level_dbfs() const {
    if (!impl_ || !impl_->has_signal_level) return std::nullopt;
    return 10.0f * std::log10(std::max(impl_->signal_power, 1.0e-12f));
}

void ReceiveDemodulator::reset() {
    if (!impl_) {
        return;
    }
    if (impl_->amplitude_demodulator) {
        ampmodem_reset(impl_->amplitude_demodulator);
    }
    if (impl_->cw_oscillator) {
        nco_crcf_reset(impl_->cw_oscillator);
        nco_crcf_set_frequency(impl_->cw_oscillator, impl_->cw_frequency);
    }
    if (impl_->cw_filter) iirfilt_crcf_reset(impl_->cw_filter);
    if (impl_->audio_filter) {
        firfilt_rrrf_reset(impl_->audio_filter);
    }
    if (impl_->dc_blocker) {
        iirfilt_rrrf_reset(impl_->dc_blocker);
    }
    firfilt_crcf_reset(impl_->signal_filter);
    nco_crcf_reset(impl_->signal_mixer);
    nco_crcf_set_frequency(impl_->signal_mixer, impl_->signal_mixer_frequency);
    impl_->signal_power = 0.0f;
    impl_->has_signal_level = false;
}

} // namespace platform::sdr