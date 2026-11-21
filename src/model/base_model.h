/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace model {

enum class AppPage {
    Apple  = 0,
    Butter = 1,
};

enum class RadioMode {
    Usb = 0,
    Lsb,
    Am,
    Cw,
};

enum class RadioConnectionState {
    Disconnected = 0,
    Receiving,
    Transmitting,
};

enum class RadioDiscoveryState {
    Searching,
    NotFound,
    Found,
    Error,
};

struct RadioMeasurements {
    std::optional<int> signal_level_dbfs;
    std::optional<bool> ptt;
    // Microphone peak while transmitting a voice mode; -120 means no microphone.
    std::optional<int> mic_level_dbfs;
    std::optional<bool> adc_overload;
    std::optional<float> temperature_celsius;
    std::optional<uint16_t> forward_power_raw;
    std::optional<uint16_t> reverse_power_raw;
    std::optional<float> pa_current_amps;
    std::optional<float> forward_watts;
    std::optional<float> swr;
};

class BaseModel {
public:
    const char* app_title() const;
    const char* greeting() const;
    const char* radio_mode_name() const;
    const char* radio_mode_description() const;
    const char* band_name() const;
    bool transmit_allowed() const;
    uint64_t frequency_hz() const;
    uint32_t tuning_step_hz() const;
    uint32_t filter_bandwidth_hz() const;
    const RadioMeasurements& radio_measurements() const;
    void set_radio_measurements(const RadioMeasurements& measurements);

    static constexpr int kSettingRows = 6;
    int tx_drive() const;
    int mic_gain() const;
    int sidetone_hz() const;
    int tx_low_cut_hz() const;
    void set_tx_low_cut_hz(int hz);
    // 0 = Tab, 1 = Space.
    int ptt_key() const;
    void set_ptt_key(int key);
    const std::string& radio_address() const;
    int selected_setting() const;
    void set_settings(int tx_drive, int mic_gain, int sidetone_hz);
    void set_radio_address(const std::string& address);
    void select_setting(int direction);
    void adjust_setting(int direction);

    bool dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

    AppPage current_page() const;
    void set_current_page(AppPage page);
    void toggle_page();
    RadioMode radio_mode() const;
    void cycle_radio_mode(int direction);
    bool tune_frequency(int direction);
    void cycle_band(int direction);
    void cycle_tuning_step(int direction);
    bool auto_notch() const { return auto_notch_; }
    void toggle_auto_notch() { auto_notch_ = !auto_notch_; }
    // Receive offset in Hz (RIT); transmit is unaffected.
    int rit_hz() const { return rit_hz_; }
    void adjust_rit(int direction);
    void clear_rit() { rit_hz_ = 0; }
    bool rit_active() const { return rit_active_; }
    void toggle_rit() {
        rit_active_ = !rit_active_;
        if (!rit_active_) rit_hz_ = 0;
    }
    void cycle_filter_bandwidth(int direction);

private:
    bool dark_mode_ = true;
    AppPage current_page_ = AppPage::Apple;
    RadioMode radio_mode_ = RadioMode::Usb;
    uint8_t band_index_{5};
    uint8_t tuning_step_index_{2};
    uint64_t frequency_hz_{14200000};
    uint32_t filter_bandwidth_hz_{2800};
    RadioMeasurements radio_measurements_{};
    int tx_drive_{15};
    int mic_gain_{60};
    int sidetone_hz_{700};
    int tx_low_cut_hz_{300};
    int ptt_key_{0};
    bool auto_notch_{false};
    int rit_hz_{0};
    bool rit_active_{false};
    int selected_setting_{0};
    std::string radio_address_;
};

} // namespace model
