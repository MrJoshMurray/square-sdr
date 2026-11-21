/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "asset_manager.h"
#include "audio_input.h"
#include "audio_output.h"
#include "help_popup.h"
#include "logger.h"
#include "linux_input.h"
#include "screenshot_service.h"
#include "screen_manager.h"
#include "base_viewmodel.h"
#include "sdr/metis_discovery.h"
#include "sdr/metis_stream.h"
#include "sdr/radio_settings_adapter.h"
#include "sdr/receive_audio_pipeline.h"
#include "sdr/receive_demodulator.h"
#include "sdr/spectrum_simulator.h"
#include "sdr/voice_modulator.h"
#include "theme.h"
#include "toast.h"

#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if USE_DESKTOP
#include "desktop_simulator_frame.h"
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif

#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif

#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

#ifndef APP_CONFIG_FILE
#define APP_CONFIG_FILE "square-sdr.conf"
#endif

namespace app {
namespace {

void quit_requested_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* running = static_cast<bool*>(lv_observer_get_user_data(observer));
    if (running && lv_subject_get_int(subject)) {
        *running = false;
    }
}

struct DarkModePersistence {
    std::string config_path;
    bool last_dark_mode;
    ApplicationConfig settings;
};

void save_settings(DarkModePersistence& persistence) {
    persistence.settings.dark_mode = persistence.last_dark_mode;
    std::string error;
    if (!save_application_config(persistence.config_path, persistence.settings, error)) {
        LOG_WARN("failed to save config {}: {}", persistence.config_path, error);
    }
}

class SimulatedAudioWorker {
public:
    ~SimulatedAudioWorker() {
        stop();
    }

    bool start(std::string& error) {
        if (!output_.open("", 48000, error)) {
            return false;
        }
        running_.store(true);
        worker_ = std::thread(&SimulatedAudioWorker::run, this);
        return true;
    }

    void stop() {
        running_.store(false);
        if (worker_.joinable()) {
            worker_.join();
        }
        output_.close();
    }

    void request_band_edge_tone() {
        output_.request_band_edge_tone();
    }

private:
    void run() {
        constexpr size_t kFramesPerWrite = 960;
        constexpr uint32_t kSampleRateHz = 48000;
        constexpr float kToneHz = 700.0f;
        constexpr float kPi = 3.14159265358979323846f;
        std::array<int16_t, kFramesPerWrite> samples{};
        uint64_t sample_index = 0;

        while (running_.load()) {
            for (size_t index = 0; index < samples.size(); ++index) {
                const float phase = 2.0f * kPi * kToneHz
                                  * static_cast<float>(sample_index + index)
                                  / static_cast<float>(kSampleRateHz);
                samples[index] = static_cast<int16_t>(std::lrint(std::sin(phase) * 4915.0f));
            }

            std::string error;
            if (!output_.write(samples.data(), samples.size(), error)) {
                LOG_WARN("simulated audio output stopped: {}", error);
                running_.store(false);
                break;
            }
            sample_index += samples.size();
#if USE_DESKTOP
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
#endif
        }
    }

    platform::audio::AudioOutput output_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

class RadioReceiveWorker {
public:
    ~RadioReceiveWorker() {
        stop();
    }

    void start() {
        running_.store(true);
        worker_ = std::thread(&RadioReceiveWorker::run, this);
    }

    void set_auto_notch(bool enabled) { auto_notch_.store(enabled); }
    void set_rit_hz(int hz) { rit_hz_.store(hz); }

    void set_frequency(uint64_t frequency_hz) {
        frequency_hz_.store(frequency_hz);
    }

    void request_band_edge_tone() {
        output_.request_band_edge_tone();
    }

    void set_transmit_mode(model::RadioMode mode) {
        transmit_mode_.store(static_cast<int>(mode));
    }

    void set_mic_gain(int percent) {
        mic_gain_.store(std::clamp(percent, 0, 100));
    }

    void set_tx_low_cut_hz(int hz) {
        tx_low_cut_hz_.store(std::clamp(hz, 100, 300));
    }

    void set_transmit_allowed(bool allowed) {
        transmit_allowed_.store(allowed);
    }

    void request_transmit(bool requested) {
        transmit_requested_.store(requested && transmit_allowed_.load());
    }

    bool transmit_requested() const {
        return transmit_requested_.load();
    }

    bool transmitting() const {
        return transmitting_.load();
    }

    void set_transmit_drive(int level) {
        transmit_drive_.store(static_cast<uint8_t>(std::clamp(level, 0, 15) << 4));
    }

    void set_sidetone_hz(int hz) {
        sidetone_hz_.store(std::clamp(hz, 100, 3000));
    }

    std::string radio_address() const {
        std::lock_guard<std::mutex> lock(address_mutex_);
        return radio_address_;
    }

    bool take_swr_fault() {
        return swr_fault_.exchange(false);
    }

    void set_receive_settings(model::RadioMode mode, uint32_t bandwidth_hz) {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        desired_settings_ = platform::sdr::make_demodulator_settings(mode, bandwidth_hz);
    }

    model::RadioConnectionState status() const {
        return static_cast<model::RadioConnectionState>(status_.load());
    }

    model::RadioDiscoveryState discovery_state() const {
        return static_cast<model::RadioDiscoveryState>(discovery_state_.load());
    }

    model::RadioMeasurements measurements() const {
        std::lock_guard<std::mutex> lock(measurements_mutex_);
        if (std::chrono::steady_clock::now() - last_measurement_ > std::chrono::seconds(1)) {
            return {};
        }
        return measurements_;
    }

    void stop() {
        running_.store(false);
        retry_ready_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
        stream_.stop();
        output_.close();
        mic_.close();
    }

private:
    void run() {
        std::vector<platform::sdr::MetisRadioEndpoint> radios;
        std::string error;
        while (running_.load()) {
            const bool discovered = platform::sdr::discover_metis_radios(1500, radios, error);
            if (!running_.load()) return;
            if (discovered && !radios.empty()) {
                discovery_state_.store(static_cast<int>(model::RadioDiscoveryState::Found));
                break;
            }
            discovery_state_.store(static_cast<int>(discovered ? model::RadioDiscoveryState::NotFound
                                                               : model::RadioDiscoveryState::Error));
            LOG_WARN("live radio discovery failed: {}", error.empty() ? "no radio replied" : error);
            std::unique_lock<std::mutex> lock(retry_mutex_);
            retry_ready_.wait_for(lock, std::chrono::seconds(2), [this] { return !running_.load(); });
        }
        if (!running_.load()) {
            return;
        }

        LOG_INFO("discovered Metis radio at {}:{}", radios.front().address, radios.front().port);
        {
            std::lock_guard<std::mutex> lock(address_mutex_);
            radio_address_ = radios.front().address;
        }
        if (!stream_.open(radios.front().address, error)
            || !stream_.configure_receiver(static_cast<uint32_t>(frequency_hz_.load()), error)
            || !stream_.start(error)) {
            LOG_WARN("live IQ stream failed to start: {}", error);
            running_.store(false);
            return;
        }
        {
            std::string mic_error;
            const std::string wanted_mic = platform::audio::find_preferred_capture_device();
            if (!mic_.open(wanted_mic, platform::sdr::kDefaultRxSampleRateHz, mic_error)) {
                LOG_WARN("microphone unavailable ({}): {}", wanted_mic, mic_error);
            }
        }
        if (!output_.open("", platform::sdr::kDefaultRxSampleRateHz, error)) {
            LOG_WARN("live audio output unavailable: {}", error);
            stream_.stop();
            running_.store(false);
            return;
        }
        platform::sdr::ReceiveAudioPipeline pipeline(output_);
        platform::sdr::DemodulatorSettings settings;
        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            settings = desired_settings_;
        }
        if (!pipeline.configure(settings, error)) {
            LOG_WARN("live demodulator failed to configure: {}", error);
            stream_.stop();
            output_.close();
            running_.store(false);
            return;
        }
        status_.store(static_cast<int>(model::RadioConnectionState::Receiving));

        std::array<uint8_t, 2048> packet{};
        auto last_receiver_control = std::chrono::steady_clock::now();
        auto last_publish = last_receiver_control;
        auto last_packet = last_receiver_control;
        auto transmit_started = last_receiver_control;
        uint64_t transmit_frequency_hz = 0;
        bool mox = false;
        uint32_t transmit_phase = 0;
        double sidetone_phase = 0.0;
        size_t sidetone_ramp = 0;
        size_t sidetone_written = 0;
        auto sidetone_start = std::chrono::steady_clock::now();
        auto unkeyed_at = std::chrono::steady_clock::time_point{};
        auto request_since = std::chrono::steady_clock::now();
        platform::sdr::MetisTelemetry telemetry;
        platform::sdr::VoiceModulator modulator;
        model::RadioMode tx_mode = model::RadioMode::Usb;
        bool mic_primed = false;
        int mic_peak = 0;
        std::array<int16_t, platform::sdr::kMetisIqSamplesPerPacket * 2> voice_iq{};
        auto last_mic_check = std::chrono::steady_clock::time_point{};
        int high_swr_count = 0;
        float peak_forward_watts = 0.0f;
        while (running_.load()) {
            size_t received = 0;
            if (!stream_.receive(packet.data(), packet.size(), 100, received, error)) {
                LOG_WARN("live IQ receive failed: {}", error);
                break;
            }
            {
                std::lock_guard<std::mutex> lock(settings_mutex_);
                settings = desired_settings_;
            }
            if (!pipeline.configure_if_changed(settings, error)) {
                LOG_WARN("live demodulator reconfiguration failed: {}", error);
                break;
            }
            pipeline.set_auto_notch(auto_notch_.load());
            const auto now = std::chrono::steady_clock::now();
            const uint64_t frequency_hz = frequency_hz_.load();
            // RIT shifts only the receiver; transmit stays on the dial frequency.
            const uint64_t rx_frequency_hz = static_cast<uint64_t>(static_cast<int64_t>(frequency_hz) + rit_hz_.load());
            const bool requested = transmit_requested_.load() && transmit_allowed_.load();
            if (!requested) request_since = now;
            // Voice modes wait for the speaker mute to settle so its pop is not transmitted.
            const bool voice_tx = static_cast<model::RadioMode>(transmit_mode_.load()) != model::RadioMode::Cw;
            const bool want_mox = requested
                               && (mox || !voice_tx || now - request_since >= std::chrono::milliseconds(200));
            // Follow the best available microphone (USB headset, else the on-board codec),
            // including hot-plugging, while not transmitting.
            if (!mox && now - last_mic_check > std::chrono::seconds(2)) {
                last_mic_check = now;
                const std::string wanted_mic = platform::audio::find_preferred_capture_device();
                if (!mic_.is_open() || mic_.device_id() != wanted_mic) {
                    std::string mic_error;
                    if (mic_.open(wanted_mic, platform::sdr::kDefaultRxSampleRateHz, mic_error)) {
                        LOG_INFO("microphone opened: {}", wanted_mic);
                    } else {
                        LOG_WARN("microphone unavailable ({}): {}", wanted_mic, mic_error);
                    }
                }
            }
            if (want_mox && !mox) {
                tx_mode = static_cast<model::RadioMode>(transmit_mode_.load());
                modulator.configure(tx_mode, mic_gain_.load(), tx_low_cut_hz_.load());
                modulator.reset();
                mic_.clear();
                mic_primed = false;
                mic_peak = 0;
                high_swr_count = 0;
                peak_forward_watts = 0.0f;
                transmit_started = now;
                transmit_frequency_hz = frequency_hz;
            }
            // Unkey on retune or after the transmit time limit.
            const bool keep_mox = want_mox
                               && frequency_hz == transmit_frequency_hz
                               && now - transmit_started < kMaxTransmitDuration;
            if (mox && !keep_mox) transmit_requested_.store(false);
            const bool mox_changed = keep_mox != mox;
            mox = keep_mox;
            if (mox_changed) {
                output_.flush();
                sidetone_ramp = 0;
                sidetone_start = now;
                sidetone_written = 0;
            }
            if (mox_changed && !mox) unkeyed_at = now;
            // The receiver still sees the transmit tail for a moment after unkeying.
            const bool spectrum_hold = mox || now - unkeyed_at < std::chrono::milliseconds(300);
            platform::sdr::LiveSpectrumBuffer::set_blanked(spectrum_hold);
            transmitting_.store(mox);
            status_.store(static_cast<int>(mox ? model::RadioConnectionState::Transmitting
                                               : model::RadioConnectionState::Receiving));
            if (mox_changed || (!mox && now - last_receiver_control >= std::chrono::milliseconds(100))) {
                platform::sdr::MetisTransmitControl transmit;
                transmit.mox = mox;
                transmit.drive = transmit_drive_.load();
                transmit.release = mox_changed;
                const uint64_t control_hz = mox ? frequency_hz : rx_frequency_hz;
                if (!stream_.configure_receiver(static_cast<uint32_t>(control_hz), error, transmit)) {
                    LOG_WARN("live receiver control heartbeat failed: {}", error);
                    break;
                }
                last_receiver_control = now;
            }
            if (received != 0) {
                platform::sdr::MetisIqPacket parsed;
                const auto parse_result = platform::sdr::parse_metis_ep6_packet(
                    packet.data(), received, parsed);
                if (parse_result == platform::sdr::MetisPacketError::None) {
                    if (mox) {
                        platform::sdr::MetisTransmitControl transmit;
                        transmit.mox = true;
                        transmit.drive = transmit_drive_.load();
                        transmit.carrier = tx_mode == model::RadioMode::Cw;
                        if (!transmit.carrier) {
                            // Hold back about 10 ms of microphone audio so capture jitter never
                            // starves the transmitter; re-prime after any underrun.
                            constexpr size_t kPrime = 480;
                            constexpr size_t kCount = platform::sdr::kMetisIqSamplesPerPacket;
                            std::array<int16_t, kCount> mic_block{};
                            if (!mic_primed && mic_.available() >= kPrime) mic_primed = true;
                            if (mic_primed && mic_.available() < kCount) mic_primed = false;
                            if (mic_primed) {
                                mic_.trim(2400);
                                mic_.read(mic_block.data(), kCount);
                                for (const int16_t sample : mic_block) mic_peak = std::max(mic_peak, std::abs(static_cast<int>(sample)));
                            }
                            modulator.configure(tx_mode, mic_gain_.load(), tx_low_cut_hz_.load());
                            modulator.process(mic_block.data(), kCount, voice_iq.data());
                            transmit.iq = voice_iq.data();
                        }
                        if (!stream_.send_transmit_frame(static_cast<uint32_t>(frequency_hz),
                                                         transmit,
                                                         transmit_phase++,
                                                         error)) {
                            LOG_WARN("live transmit frame failed: {}", error);
                            break;
                        }
                    }
                    if (now - last_packet > std::chrono::seconds(1)) telemetry = {};
                    last_packet = now;
                    telemetry.update(parsed);
                    // Mute the receive chain while keyed so the radio's own transmit leakage
                    // does not saturate the waterfall or play through the speaker.
                    bool processed = true;
                    if (!mox) {
                        if (!spectrum_hold) {
                            platform::sdr::LiveSpectrumBuffer::push_iq(parsed.samples.data(),
                                                                       parsed.sample_count);
                        }
                        processed = pipeline.process_datagram(packet.data(), received, error);
                        if (!processed) LOG_WARN("live EP6 packet rejected: {}", error);
                    } else if (tx_mode == model::RadioMode::Cw) {
                        // CW sidetone, 700 Hz. Keep a small cushion of about 20 ms queued: enough
                        // to ride out packet jitter without crackle, short enough to feel instant.
                        constexpr size_t kTargetFrames = 960;
                        // Pace against the wall clock rather than the device's reported delay,
                        // which some ALSA plugins do not report reliably.
                        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            now - sidetone_start).count();
                        const size_t target = static_cast<size_t>(elapsed_us * 48 / 1000) + kTargetFrames;
                        const size_t wanted = target > sidetone_written ? target - sidetone_written : 0;
                        std::array<int16_t, 256> tone{};
                        const size_t count = std::min(wanted, tone.size());
                        for (size_t i = 0; i < count; ++i) {
                            // 2 ms fade-in avoids a click at key-down.
                            const double gain = std::min(1.0, static_cast<double>(sidetone_ramp++) / 96.0);
                            tone[i] = static_cast<int16_t>(3000.0 * gain * std::sin(sidetone_phase));
                            sidetone_phase += 2.0 * 3.14159265358979 * sidetone_hz_.load() / 48000.0;
                            if (sidetone_phase > 2.0 * 3.14159265358979) {
                                sidetone_phase -= 2.0 * 3.14159265358979;
                            }
                        }
                        sidetone_written += count;
                        if (count != 0) {
                            std::string tone_error;
                            output_.write(tone.data(), count, tone_error);
                            output_.start_now();
                        }
                    }
                    if (spectrum_hold) {
                        // Keep the waterfall quiet while the receiver hears our own signal.
                        static const std::array<platform::sdr::IqSample, 126> silence{};
                        platform::sdr::LiveSpectrumBuffer::push_iq(
                            silence.data(), std::min<size_t>(parsed.sample_count, silence.size()));
                    }
                    if (mox && now - transmit_started > std::chrono::milliseconds(200)) {
                        // Protect the PA: trip after a short run of readings at SWR 3:1 or worse.
                        const auto power = platform::sdr::estimate_metis_power(telemetry);
                        const bool reflected = power.forward_watts && *power.forward_watts > 2.0f
                                            && telemetry.reverse_power_raw && telemetry.forward_power_raw
                                            && *telemetry.reverse_power_raw >= *telemetry.forward_power_raw;
                        // The radio's own foldback can cut power instead of showing a high SWR,
                        // so also trip on strong reflected power or a collapse in forward power.
                        float reverse_watts = 0.0f;
                        if (telemetry.reverse_power_raw && *telemetry.reverse_power_raw <= 4095) {
                            const float volts = static_cast<float>(std::max(0, static_cast<int>(*telemetry.reverse_power_raw) - 6))
                                              * (3.3f / 4095.0f);
                            reverse_watts = volts * volts / 1.5f;
                        }
                        const float forward_watts = power.forward_watts.value_or(0.0f);
                        peak_forward_watts = std::max(peak_forward_watts, forward_watts);
                        // Voice power naturally collapses in speech pauses, so only a steady carrier
                        // can be judged this way.
                        const bool collapsed = tx_mode == model::RadioMode::Cw
                                            && peak_forward_watts > 2.0f
                                            && forward_watts < peak_forward_watts * 0.6f;
                        const bool high_swr = reflected || collapsed || reverse_watts >= kMaxReverseWatts
                                           || (power.swr && *power.swr >= kMaxSwr);
                        high_swr_count = high_swr ? high_swr_count + 1 : 0;
                        if (high_swr_count >= kSwrTripCount) {
                            swr_fault_.store(true);
                            transmit_requested_.store(false);
                            high_swr_count = 0;
                        }
                    }
                    if (processed && now - last_publish >= std::chrono::milliseconds(100)) {
                        model::RadioMeasurements next;
                        if (const auto level = pipeline.signal_level_dbfs()) {
                            next.signal_level_dbfs = static_cast<int>(std::lround(*level));
                        }
                        next.ptt = mox;
                        if (mox && tx_mode != model::RadioMode::Cw) {
                            next.mic_level_dbfs = !mic_.is_open() ? -120
                                : mic_peak <= 0 ? -99
                                : static_cast<int>(std::lround(20.0 * std::log10(mic_peak / 32768.0)));
                            mic_peak = 0;
                        }
                        next.adc_overload = telemetry.adc_overload;
                        next.temperature_celsius = platform::sdr::metis_temperature_celsius(telemetry.temperature_raw);
                        next.forward_power_raw = telemetry.forward_power_raw;
                        next.reverse_power_raw = telemetry.reverse_power_raw;
                        next.pa_current_amps = platform::sdr::metis_pa_current_amps(telemetry.pa_current_raw);
                        const auto power = platform::sdr::estimate_metis_power(telemetry);
                        next.forward_watts = power.forward_watts;
                        next.swr = power.swr;
                        std::lock_guard<std::mutex> lock(measurements_mutex_);
                        measurements_ = next;
                        last_measurement_ = now;
                        last_publish = now;
                    }
                }
            }
        }
        transmitting_.store(false);
        platform::sdr::LiveSpectrumBuffer::set_blanked(false);
        std::string unkey_error;
        stream_.configure_receiver(static_cast<uint32_t>(frequency_hz_.load()), unkey_error, {false, 0, true});
        stream_.stop();
        output_.close();
        status_.store(static_cast<int>(model::RadioConnectionState::Disconnected));
        std::lock_guard<std::mutex> lock(measurements_mutex_);
        measurements_ = {};
        last_measurement_ = {};
    }

    static constexpr auto kMaxTransmitDuration = std::chrono::minutes(3);
    std::atomic<uint8_t> transmit_drive_{0xF0};
    std::atomic<int> sidetone_hz_{700};
    mutable std::mutex address_mutex_;
    std::string radio_address_;

    platform::audio::AudioOutput output_;
    platform::sdr::MetisIqStream stream_;
    std::atomic<bool> transmit_requested_{false};
    std::atomic<bool> transmit_allowed_{false};
    std::atomic<int> transmit_mode_{static_cast<int>(model::RadioMode::Usb)};
    std::atomic<int> mic_gain_{60};
    std::atomic<int> tx_low_cut_hz_{300};
    platform::audio::AudioInput mic_;
    std::atomic<bool> transmitting_{false};
    std::atomic<bool> swr_fault_{false};
    static constexpr float kMaxSwr = 3.0f;
    static constexpr float kMaxReverseWatts = 1.0f;
    static constexpr int kSwrTripCount = 3;
    std::atomic<bool> running_{false};
    std::atomic<int> status_{static_cast<int>(model::RadioConnectionState::Disconnected)};
    std::atomic<int> discovery_state_{static_cast<int>(model::RadioDiscoveryState::Searching)};
    std::atomic<uint64_t> frequency_hz_{14200000};
    std::atomic<bool> auto_notch_{false};
    std::atomic<int> rit_hz_{0};
    std::mutex settings_mutex_;
    platform::sdr::DemodulatorSettings desired_settings_{};
    mutable std::mutex measurements_mutex_;
    model::RadioMeasurements measurements_{};
    std::chrono::steady_clock::time_point last_measurement_{};
    std::mutex retry_mutex_;
    std::condition_variable retry_ready_;
    std::thread worker_;
};

std::string writable_config_path() {
#if USE_DESKTOP
    return APP_CONFIG_FILE;
#else
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        const std::filesystem::path root(xdg_config_home);
        if (!root.empty() && root.is_absolute()) {
            return (root / "square-sdr" / "square-sdr.conf").string();
        }
    }
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path root(home);
        if (!root.empty() && root.is_absolute()) {
            return (root / ".config" / "square-sdr" / "square-sdr.conf").string();
        }
    }
    return APP_CONFIG_FILE;
#endif
}

void persist_dark_mode_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* persistence = static_cast<DarkModePersistence*>(lv_observer_get_user_data(observer));
    if (!persistence) {
        return;
    }
    const bool dark_mode = lv_subject_get_int(subject) != 0;
    if (dark_mode == persistence->last_dark_mode) {
        return;
    }
    persistence->last_dark_mode = dark_mode;

    save_settings(*persistence);
    LOG_INFO("saved config: {} (dark_mode={})",
             persistence->config_path,
             dark_mode ? "yes" : "no");
}

class ShortcutController {
public:
    ShortcutController(viewmodel::BaseViewModel& view_model, AssetManager& assets)
        : view_model_(view_model),
          toast_(std::make_unique<view::widgets::Toast>(lv_layer_top(), view_model, assets)),
                    help_popup_(std::make_unique<view::widgets::HelpPopup>(lv_layer_top(), view_model, assets)),
                    volume_overlay_(std::make_unique<view::widgets::VolumeOverlay>(lv_layer_top(), assets)) {
        toast_->build();
        help_popup_->build();
                volume_overlay_->build();
        tune_repeat_timer_ = lv_timer_create(tune_repeat_timer_cb, 120, this);
        lv_timer_pause(tune_repeat_timer_);
        volume_error_timer_ = lv_timer_create(volume_error_timer_cb, 250, this);
        platform::set_global_key_listener(global_key_listener, this);
        platform::set_key_release_listener(key_release_listener, this);
    }

    ~ShortcutController() {
        if (volume_error_timer_) {
            lv_timer_delete(volume_error_timer_);
        }
        if (tune_repeat_timer_) {
            lv_timer_delete(tune_repeat_timer_);
            tune_repeat_timer_ = nullptr;
        }
        platform::clear_key_release_listener(key_release_listener, this);
        platform::clear_global_key_listener(global_key_listener, this);
    }

    void set_band_edge_handler(std::function<void()> handler) {
        band_edge_handler_ = std::move(handler);
    }

    void show_warning(const std::string& text) {
        toast_->show(text, view::widgets::ToastTone::Warning, 2500);
    }

    // The built-in speaker amplifier couples into the built-in microphone while transmitting
    // voice, so mute it for the duration of the transmission and restore it afterwards.
    void set_transmit_mute(bool transmitting) {
        if (transmitting && !tx_muted_ && !user_muted_) {
            tx_muted_ = system_volume_.request(platform::audio::VolumeAction::Mute);
        } else if (!transmitting && tx_muted_) {
            if (system_volume_.request(platform::audio::VolumeAction::Unmute)) tx_muted_ = false;
        }
    }

    void show_error(const std::string& text, uint32_t duration_ms) {
        toast_->show(text, view::widgets::ToastTone::Alarm, duration_ms);
    }

    void set_ptt_key(uint32_t key) { ptt_key_ = key; }

    void set_transmit_handler(std::function<void(bool)> handler) {
        transmit_handler_ = std::move(handler);
    }

private:
    void tune(int direction) {
        if (view_model_.tune_frequency(direction) && band_edge_handler_) {
            band_edge_handler_();
        }
    }

    static void volume_error_timer_cb(lv_timer_t* timer) {
        auto* controller = static_cast<ShortcutController*>(lv_timer_get_user_data(timer));
        if (!controller) return;
        platform::audio::VolumeState state;
        if (controller->system_volume_.take_state(state) && !state.silent) {
            controller->user_muted_ = state.muted;
            controller->volume_overlay_->show(state.percent, state.muted);
        }
        std::string error;
        if (controller->system_volume_.take_error(error)) {
            LOG_WARN("system audio control failed: {}", error);
            controller->toast_->show("Audio control unavailable",
                                     view::widgets::ToastTone::Error,
                                     3000);
        }
    }

    static void tune_repeat_timer_cb(lv_timer_t* timer) {
        auto* controller = static_cast<ShortcutController*>(lv_timer_get_user_data(timer));
        if (!controller || controller->tune_repeat_key_ == 0
            || controller->view_model_.current_page() != model::AppPage::Apple) {
            if (controller) {
                lv_timer_pause(timer);
            }
            return;
        }

        if (controller->tune_repeat_delay_ticks_ > 0) {
            --controller->tune_repeat_delay_ticks_;
            return;
        }

        const bool left = controller->tune_repeat_key_ == LV_KEY_LEFT
                       || controller->tune_repeat_key_ == 'z'
                       || controller->tune_repeat_key_ == 'Z';
        controller->tune(left ? -1 : 1);
    }

    void start_tune_repeat(uint32_t key) {
        tune_repeat_key_ = key;
        tune_repeat_delay_ticks_ = 3;
        if (tune_repeat_timer_) {
            lv_timer_reset(tune_repeat_timer_);
            lv_timer_resume(tune_repeat_timer_);
        }
    }

    void stop_tune_repeat(uint32_t key) {
        if (key == tune_repeat_key_) {
            tune_repeat_key_ = 0;
            tune_repeat_delay_ticks_ = 0;
            if (tune_repeat_timer_) {
                lv_timer_pause(tune_repeat_timer_);
            }
        }
    }

    bool handle_key(uint32_t key, bool long_pressed) {
        if (key == platform::kKeyMute || key == platform::kKeyVolumeDown
            || key == platform::kKeyVolumeUp) {
            if (!long_pressed) {
                const auto action = key == platform::kKeyMute
                                  ? platform::audio::VolumeAction::ToggleMute
                                  : key == platform::kKeyVolumeDown
                                  ? platform::audio::VolumeAction::Down
                                  : platform::audio::VolumeAction::Up;
                if (!system_volume_.request(action)) {
                    toast_->show("Audio control unavailable", view::widgets::ToastTone::Error, 3000);
                }
            }
            return true;
        }

        if (key == platform::kKeyPrintScreen) {
            if (!long_pressed) {
                const auto result = platform::screenshot::capture_active_screen();
                if (result.success) {
                    LOG_INFO("screenshot saved: {}", result.path);
                    toast_->show_highlighted("Saved to ",
                                             result.path,
                                             "",
                                             view::widgets::ToastTone::Success,
                                             5200,
                                             true);
                }
                else {
                    LOG_WARN("screenshot failed: {}", result.error);
                    toast_->show("Screenshot failed: " + result.error,
                                 view::widgets::ToastTone::Error,
                                 3200);
                }
            }
            return true;
        }

        if (key == platform::kKeyHelp) {
            if (!long_pressed) {
                toast_->hide();
                if (help_popup_->visible()) {
                    help_popup_->hide();
                }
                else {
                    help_popup_->show(view_model_.current_page());
                }
            }
            return true;
        }

        if (help_popup_->visible()) {
            if (key == LV_KEY_ESC && !long_pressed) {
                help_popup_->hide();
            }
            return true;
        }

        if (!long_pressed) {
            if (key == ptt_key_ && transmit_handler_) {
                transmit_handler_(true);
                return true;
            }
            if (key == 'u' || key == 'U') {
                view_model_.set_dark_mode(true);
                return true;
            }
            if (key == 'i' || key == 'I') {
                view_model_.set_dark_mode(false);
                return true;
            }
            const auto page = view_model_.current_page();
            const bool left = key == LV_KEY_LEFT || key == 'z' || key == 'Z';
            const bool right = key == LV_KEY_RIGHT || key == 'c' || key == 'C';

            if (page == model::AppPage::Apple) {
                if (left || right) {
                    tune(left ? -1 : 1);
                    start_tune_repeat(key);
                    return true;
                }
                if (key == 'n' || key == 'N') {
                    view_model_.toggle_auto_notch();
                    return true;
                }
                if (key == 'r' || key == 'R') {
                    view_model_.toggle_rit();
                    return true;
                }
                if (view_model_.rit_active()) {
                    const bool up = key == LV_KEY_UP || key == 'f' || key == 'F';
                    const bool down = key == LV_KEY_DOWN || key == 'x' || key == 'X';
                    if (up || down) {
                        view_model_.adjust_rit(up ? 1 : -1);
                        return true;
                    }
                }
            }
            else {
                if (left || right) {
                    view_model_.adjust_setting(left ? -1 : 1);
                    return true;
                }
                const bool up = key == LV_KEY_UP || key == 'f' || key == 'F';
                const bool down = key == LV_KEY_DOWN || key == 'x' || key == 'X';
                if (up || down) {
                    view_model_.select_setting(up ? -1 : 1);
                    return true;
                }
            }
        }

        const bool is_escape = key == LV_KEY_ESC;
        if (!is_escape) {
            return false;
        }

        if (long_pressed) {
            if (exit_key_pressed_ == key && exit_armed_) {
                toast_->hide();
                view_model_.request_quit();
            }
        }
        else {
            exit_key_pressed_ = key;
            exit_armed_ = view_model_.current_page() == model::AppPage::Apple;
            if (exit_armed_) {
                toast_->show_persistent_highlighted("Hold ",
                                                    "ESC",
                                                    " to exit",
                                                    view::widgets::ToastTone::Warning);
            }
        }
        return true;
    }

    void handle_release(uint32_t key) {
        if (key == ptt_key_ && transmit_handler_) transmit_handler_(false);
        stop_tune_repeat(key);
        if (key != exit_key_pressed_) {
            return;
        }

        if (exit_armed_) {
            toast_->hide();
        } else if (key == LV_KEY_ESC &&
                   view_model_.current_page() == model::AppPage::Butter) {
            view_model_.show_apple_page();
        }
        exit_key_pressed_ = 0;
        exit_armed_ = false;
    }

    static bool global_key_listener(uint32_t key,
                                    const char*,
                                    bool long_pressed,
                                    void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        return controller && controller->handle_key(key, long_pressed);
    }

    static void key_release_listener(uint32_t key, const char*, void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        if (controller) {
            controller->handle_release(key);
        }
    }

    viewmodel::BaseViewModel& view_model_;
    std::function<void()> band_edge_handler_;
    std::function<void(bool)> transmit_handler_;
    uint32_t ptt_key_{LV_KEY_NEXT};
    std::unique_ptr<view::widgets::Toast> toast_;
    std::unique_ptr<view::widgets::HelpPopup> help_popup_;
    std::unique_ptr<view::widgets::VolumeOverlay> volume_overlay_;
    platform::audio::SystemVolumeControl system_volume_;
    lv_timer_t* volume_error_timer_{nullptr};
    bool user_muted_{false};
    bool tx_muted_{false};
    lv_timer_t* tune_repeat_timer_{nullptr};
    uint32_t tune_repeat_key_{0};
    uint8_t tune_repeat_delay_ticks_{0};
    uint32_t exit_key_pressed_{0};
    bool exit_armed_{false};
};

#if !USE_DESKTOP
lv_display_t* init_device_display() {
#if APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#endif
}
#endif

} // namespace

int Application::run() {
    logger::Logger::init();
    logger::Logger::set_tag("template");

#if !USE_DESKTOP
    if (setenv("SQUARE_SDR_LIVE_RADIO", "1", 0) != 0) {
        LOG_ERROR("failed to enable live radio mode");
        return 1;
    }
#endif

    lv_init();

    AssetManager assets;
    for (const auto& root : assets.roots()) {
        LOG_INFO("asset root: {}", root.string());
    }

    viewmodel::BaseViewModel view_model;
    const std::string user_config_path = writable_config_path();
    std::string loaded_config_path = APP_CONFIG_FILE;
    if (user_config_path != APP_CONFIG_FILE) {
        std::error_code filesystem_error;
        if (std::filesystem::is_regular_file(user_config_path, filesystem_error)) {
            loaded_config_path = user_config_path;
        }
    }

    ApplicationConfig config;
    std::string config_error;
    if (load_application_config(loaded_config_path, config, config_error)) {
        view_model.set_dark_mode(config.dark_mode);
        view_model.set_settings(config.tx_drive, config.mic_gain, config.sidetone_hz);
        view_model.set_tx_low_cut_hz(config.tx_low_cut_hz);
        view_model.set_ptt_key(config.ptt_key);
        LOG_INFO("loaded config: {} (dark_mode={})",
                 loaded_config_path,
                 config.dark_mode ? "yes" : "no");
    } else {
        LOG_WARN("failed to load config {}: {}; using defaults", loaded_config_path, config_error);
    }

#if USE_DESKTOP
    DesktopSimulatorFrame simulator_frame(assets);
    auto* display = simulator_frame.display();
#else
    auto* display = init_device_display();
#endif
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }

    auto* standard_font = assets.load_standard_font(14);
    view::apply_lvgl_theme(display,
                           view_model.is_dark_mode(),
                           standard_font ? standard_font : LV_FONT_DEFAULT);

#if USE_DESKTOP
    simulator_frame.bind_dark_mode(view_model.dark_mode_subject());
#endif

    ScreenManager screen_manager(view_model, assets);
    screen_manager.start();
    ShortcutController shortcuts(view_model, assets);
    view::widgets::RadioDiscoveryNotice discovery_notice(lv_layer_top(), view_model, assets);
    discovery_notice.build();

    DarkModePersistence dark_mode_persistence{user_config_path, view_model.is_dark_mode(), {}};
    dark_mode_persistence.settings.tx_drive = view_model.tx_drive();
    dark_mode_persistence.settings.mic_gain = view_model.mic_gain();
    dark_mode_persistence.settings.sidetone_hz = view_model.sidetone_hz();
    dark_mode_persistence.settings.tx_low_cut_hz = view_model.tx_low_cut_hz();
    dark_mode_persistence.settings.ptt_key = view_model.ptt_key();
    auto* dark_mode_observer = lv_subject_add_observer(view_model.dark_mode_subject(),
                                                        persist_dark_mode_observer,
                                                        &dark_mode_persistence);

    bool running = true;
    auto* quit_observer = lv_subject_add_observer(view_model.quit_requested_subject(),
                                                  quit_requested_observer,
                                                  &running);

    std::unique_ptr<SimulatedAudioWorker> simulated_audio;
    std::unique_ptr<RadioReceiveWorker> radio_audio;
    if (std::getenv("SQUARE_SDR_LIVE_RADIO")) {
        radio_audio = std::make_unique<RadioReceiveWorker>();
        radio_audio->set_frequency(view_model.frequency_hz());
        radio_audio->set_receive_settings(view_model.radio_mode(), view_model.filter_bandwidth_hz());
        radio_audio->start();
        LOG_INFO("live radio mode enabled; hold Tab to transmit");
    } else {
        simulated_audio = std::make_unique<SimulatedAudioWorker>();
        std::string audio_error;
        if (simulated_audio->start(audio_error)) {
            LOG_INFO("simulated audio output opened at 48 kHz");
        } else {
            LOG_WARN("simulated audio output unavailable: {}", audio_error);
        }
    }

    shortcuts.set_band_edge_handler([&] {
        if (radio_audio) radio_audio->request_band_edge_tone();
        if (simulated_audio) simulated_audio->request_band_edge_tone();
    });

    bool ptt = false;
    if (radio_audio) {
        shortcuts.set_transmit_handler([&](bool pressed) {
            if (pressed && !view_model.transmit_allowed()) {
                shortcuts.show_warning("TX blocked: outside allocation");
                return;
            }
            ptt = pressed;
            radio_audio->set_transmit_allowed(view_model.transmit_allowed());
            radio_audio->request_transmit(ptt);
        });
    }

    LOG_INFO("LVGL app started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    while (running
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        if (radio_audio) {
            shortcuts.set_ptt_key(view_model.ptt_key() == 1 ? static_cast<uint32_t>(' ') : static_cast<uint32_t>(LV_KEY_NEXT));
            radio_audio->set_transmit_allowed(view_model.transmit_allowed());
            radio_audio->set_transmit_mode(view_model.radio_mode());
            radio_audio->set_mic_gain(view_model.mic_gain());
            shortcuts.set_transmit_mute(radio_audio->transmit_requested()
                                        && view_model.radio_mode() != model::RadioMode::Cw);
            if (ptt && !radio_audio->transmit_requested()) ptt = false;
            if (radio_audio->take_swr_fault()) {
                ptt = false;
                shortcuts.show_error("High SWR!", 5000);
            }
            radio_audio->set_transmit_drive(view_model.tx_drive());
            radio_audio->set_sidetone_hz(view_model.sidetone_hz());
            radio_audio->set_tx_low_cut_hz(view_model.tx_low_cut_hz());
            radio_audio->set_auto_notch(view_model.auto_notch());
            radio_audio->set_rit_hz(view_model.rit_hz());
            view_model.set_radio_address(radio_audio->radio_address());
            auto& saved = dark_mode_persistence.settings;
            if (saved.tx_drive != view_model.tx_drive() || saved.mic_gain != view_model.mic_gain()
                || saved.sidetone_hz != view_model.sidetone_hz()
                || saved.tx_low_cut_hz != view_model.tx_low_cut_hz()
                || saved.ptt_key != view_model.ptt_key()) {
                saved.tx_drive = view_model.tx_drive();
                saved.mic_gain = view_model.mic_gain();
                saved.sidetone_hz = view_model.sidetone_hz();
                saved.tx_low_cut_hz = view_model.tx_low_cut_hz();
                saved.ptt_key = view_model.ptt_key();
                save_settings(dark_mode_persistence);
            }
            radio_audio->set_frequency(view_model.frequency_hz());
            radio_audio->set_receive_settings(view_model.radio_mode(), view_model.filter_bandwidth_hz());
            discovery_notice.update(radio_audio->discovery_state());
            view_model.set_radio_connection_state(radio_audio->status());
            view_model.set_radio_measurements(radio_audio->measurements());
        } else {
            view_model.set_radio_connection_state(model::RadioConnectionState::Disconnected);
        }
        lv_timer_handler();
        lv_delay_ms(5);
    }

    if (quit_observer) {
        lv_observer_remove(quit_observer);
    }
    if (dark_mode_observer) {
        lv_observer_remove(dark_mode_observer);
    }

    return 0;
}

} // namespace app
