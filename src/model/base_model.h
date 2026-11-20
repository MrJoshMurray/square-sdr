/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <optional>

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
    uint64_t frequency_hz() const;
    uint32_t tuning_step_hz() const;
    uint32_t filter_bandwidth_hz() const;
    const RadioMeasurements& radio_measurements() const;
    void set_radio_measurements(const RadioMeasurements& measurements);

    bool dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

    AppPage current_page() const;
    void set_current_page(AppPage page);
    void toggle_page();
    RadioMode radio_mode() const;
    void cycle_radio_mode(int direction);
    void tune_frequency(int direction);
    void cycle_band(int direction);
    void cycle_tuning_step(int direction);
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
};

} // namespace model
