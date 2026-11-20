/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "subjects.h"
#include "base_model.h"

#include "lvgl.h"

namespace viewmodel {

class BaseViewModel {
public:
    BaseViewModel();
    ~BaseViewModel();

    BaseViewModel(const BaseViewModel&) = delete;
    BaseViewModel& operator=(const BaseViewModel&) = delete;

    lv_subject_t* title_subject();
    lv_subject_t* greeting_subject();
    lv_subject_t* radio_mode_subject();
    lv_subject_t* radio_mode_description_subject();
    lv_subject_t* frequency_subject();
    lv_subject_t* band_mode_subject();
    lv_subject_t* receiver_settings_subject();
    lv_subject_t* signal_level_subject();
    lv_subject_t* transmit_meter_subject();
    lv_subject_t* radio_detector_subject();
    lv_subject_t* radio_pa_subject();
    lv_subject_t* radio_inputs_subject();
    lv_subject_t* radio_alert_subject();
    lv_subject_t* radio_connection_state_subject();
    lv_subject_t* dark_mode_subject();
    lv_subject_t* current_page_subject();
    lv_subject_t* counter_subject();
    lv_subject_t* bold_text_subject();
    lv_subject_t* info_visible_subject();
    lv_subject_t* quit_requested_subject();

    bool is_dark_mode() const;
    uint64_t frequency_hz() const;
    model::RadioMode radio_mode() const;
    uint32_t filter_bandwidth_hz() const;
    void set_radio_connection_state(model::RadioConnectionState state);
    void set_radio_measurements(const model::RadioMeasurements& measurements);
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

    model::AppPage current_page() const;
    void show_apple_page();
    void show_butter_page();
    void toggle_page();
    void select_next_radio_mode();
    void select_previous_radio_mode();
    void tune_frequency(int direction);
    void cycle_band(int direction);
    void cycle_tuning_step(int direction);
    void cycle_filter_bandwidth(int direction);
    void increment_counter();
    void decrement_counter();
    void toggle_bold_text();
    void toggle_info();
    void request_quit();

private:
    void publish_all();

    model::BaseModel model_;
    reactive::StringSubject<32> title_subject_;
    reactive::StringSubject<32> greeting_subject_;
    reactive::StringSubject<8> radio_mode_subject_;
    reactive::StringSubject<24> radio_mode_description_subject_;
    reactive::StringSubject<24> frequency_subject_;
    reactive::StringSubject<32> band_mode_subject_;
    reactive::StringSubject<32> receiver_settings_subject_;
    reactive::StringSubject<32> signal_level_subject_;
    reactive::StringSubject<64> transmit_meter_subject_;
    reactive::StringSubject<64> radio_detector_subject_;
    reactive::StringSubject<64> radio_pa_subject_;
    reactive::StringSubject<64> radio_inputs_subject_;
    reactive::StringSubject<32> radio_alert_subject_;
    reactive::IntSubject radio_connection_state_subject_;
    reactive::BoolSubject dark_mode_subject_;
    reactive::IntSubject current_page_subject_;
    reactive::IntSubject counter_subject_;
    reactive::BoolSubject bold_text_subject_;
    reactive::BoolSubject info_visible_subject_;
    reactive::BoolSubject quit_requested_subject_;
};

} // namespace viewmodel
