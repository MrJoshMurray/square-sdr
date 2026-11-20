/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "asset_manager.h"
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
#include "theme.h"
#include "toast.h"

#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
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
};

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

    void set_frequency(uint64_t frequency_hz) {
        frequency_hz_.store(frequency_hz);
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
        if (!stream_.open(radios.front().address, error)
            || !stream_.configure_receiver(static_cast<uint32_t>(frequency_hz_.load()), error)
            || !stream_.start(error)) {
            LOG_WARN("live IQ stream failed to start: {}", error);
            running_.store(false);
            return;
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
        platform::sdr::MetisTelemetry telemetry;
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
            const auto now = std::chrono::steady_clock::now();
            if (now - last_receiver_control >= std::chrono::milliseconds(100)) {
                if (!stream_.configure_receiver(static_cast<uint32_t>(frequency_hz_.load()), error)) {
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
                    if (now - last_packet > std::chrono::seconds(1)) telemetry = {};
                    last_packet = now;
                    telemetry.update(parsed);
                    platform::sdr::LiveSpectrumBuffer::push_iq(parsed.samples.data(),
                                                               parsed.sample_count);
                    if (!pipeline.process_datagram(packet.data(), received, error)) {
                        LOG_WARN("live EP6 packet rejected: {}", error);
                    } else if (now - last_publish >= std::chrono::milliseconds(100)) {
                        model::RadioMeasurements next;
                        if (const auto level = pipeline.signal_level_dbfs()) {
                            next.signal_level_dbfs = static_cast<int>(std::lround(*level));
                        }
                        next.ptt = telemetry.ptt;
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
        stream_.stop();
        output_.close();
        status_.store(static_cast<int>(model::RadioConnectionState::Disconnected));
        std::lock_guard<std::mutex> lock(measurements_mutex_);
        measurements_ = {};
        last_measurement_ = {};
    }

    platform::audio::AudioOutput output_;
    platform::sdr::MetisIqStream stream_;
    std::atomic<bool> running_{false};
    std::atomic<int> status_{static_cast<int>(model::RadioConnectionState::Disconnected)};
    std::atomic<int> discovery_state_{static_cast<int>(model::RadioDiscoveryState::Searching)};
    std::atomic<uint64_t> frequency_hz_{14200000};
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

    ApplicationConfig config;
    config.dark_mode = dark_mode;
    std::string error;
    if (save_application_config(persistence->config_path, config, error)) {
        LOG_INFO("saved config: {} (dark_mode={})",
                 persistence->config_path,
                 config.dark_mode ? "yes" : "no");
    } else {
        LOG_WARN("failed to save config {}: {}", persistence->config_path, error);
    }
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

private:
    static void volume_error_timer_cb(lv_timer_t* timer) {
        auto* controller = static_cast<ShortcutController*>(lv_timer_get_user_data(timer));
        if (!controller) return;
        platform::audio::VolumeState state;
        if (controller->system_volume_.take_state(state)) {
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
        controller->view_model_.tune_frequency(left ? -1 : 1);
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
                    view_model_.tune_frequency(left ? -1 : 1);
                    start_tune_repeat(key);
                    return true;
                }
                if (key == 's' || key == 'S') {
                    view_model_.cycle_tuning_step(1);
                    return true;
                }
                if (key == 'b' || key == 'B') {
                    view_model_.cycle_band(1);
                    return true;
                }
                if (key == '4') {
                    view_model_.cycle_band(1);
                    return true;
                }
                if (key == 'm' || key == 'M') {
                    view_model_.select_next_radio_mode();
                    return true;
                }
                if (key == 'w' || key == 'W') {
                    view_model_.cycle_filter_bandwidth(1);
                    return true;
                }
            }
            else {
                if (left || right) {
                    left ? view_model_.select_previous_radio_mode()
                         : view_model_.select_next_radio_mode();
                    return true;
                }
                if (key == 'w' || key == 'W') {
                    view_model_.cycle_filter_bandwidth(1);
                    return true;
                }
                if (key == 'b' || key == 'B') {
                    view_model_.cycle_band(1);
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
    std::unique_ptr<view::widgets::Toast> toast_;
    std::unique_ptr<view::widgets::HelpPopup> help_popup_;
    std::unique_ptr<view::widgets::VolumeOverlay> volume_overlay_;
    platform::audio::SystemVolumeControl system_volume_;
    lv_timer_t* volume_error_timer_{nullptr};
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

    DarkModePersistence dark_mode_persistence{user_config_path, view_model.is_dark_mode()};
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
        LOG_INFO("live radio mode enabled; TX/PTT disabled");
    } else {
        simulated_audio = std::make_unique<SimulatedAudioWorker>();
        std::string audio_error;
        if (simulated_audio->start(audio_error)) {
            LOG_INFO("simulated audio output opened at 48 kHz");
        } else {
            LOG_WARN("simulated audio output unavailable: {}", audio_error);
        }
    }

    LOG_INFO("LVGL app started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    while (running
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        if (radio_audio) {
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
