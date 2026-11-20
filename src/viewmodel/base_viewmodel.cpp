/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_viewmodel.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace viewmodel {
namespace {

int page_to_int(model::AppPage page) {
    return static_cast<int>(page);
}

void format_frequency(uint64_t frequency_hz, char* output, size_t output_size) {
    std::snprintf(output,
                  output_size,
                  "%llu.%03llu.%03llu",
                  static_cast<unsigned long long>(frequency_hz / 1000000),
                  static_cast<unsigned long long>((frequency_hz / 1000) % 1000),
                  static_cast<unsigned long long>(frequency_hz % 1000));
}

void format_settings(const model::BaseModel& model, char* output, size_t output_size) {
    const uint32_t bandwidth = model.filter_bandwidth_hz();
    if (bandwidth >= 1000) {
        std::snprintf(output,
                      output_size,
                      "Step %u Hz / BW %u.%u kHz",
                      model.tuning_step_hz(),
                      bandwidth / 1000,
                      (bandwidth % 1000) / 100);
    }
    else {
        std::snprintf(output,
                      output_size,
                      "Step %u Hz / BW %u Hz",
                      model.tuning_step_hz(),
                      bandwidth);
    }
}

void format_band_mode(const model::BaseModel& model, char* output, size_t output_size) {
    std::snprintf(output,
                  output_size,
                  "%s %s",
                  model.band_name(),
                  model.radio_mode_name());
}

} // namespace

BaseViewModel::BaseViewModel()
    : title_subject_(model_.app_title()),
      greeting_subject_(model_.greeting()),
            radio_mode_subject_(model_.radio_mode_name()),
            radio_mode_description_subject_(model_.radio_mode_description()),
    frequency_subject_(""),
    band_mode_subject_(""),
    receiver_settings_subject_(""),
    signal_level_subject_("S --"),
    transmit_meter_subject_(""),
    radio_detector_subject_(""),
    radio_pa_subject_(""),
    radio_inputs_subject_(""),
    radio_alert_subject_(""),
            radio_connection_state_subject_(static_cast<int>(model::RadioConnectionState::Disconnected)),
      dark_mode_subject_(model_.dark_mode()),
      current_page_subject_(page_to_int(model_.current_page())),
      counter_subject_(0),
      bold_text_subject_(false),
    info_visible_subject_(false),
    quit_requested_subject_(false) {
    publish_all();
}

BaseViewModel::~BaseViewModel() = default;

lv_subject_t* BaseViewModel::title_subject() {
    return title_subject_.native();
}

lv_subject_t* BaseViewModel::greeting_subject() {
    return greeting_subject_.native();
}

lv_subject_t* BaseViewModel::radio_mode_subject() {
    return radio_mode_subject_.native();
}

lv_subject_t* BaseViewModel::radio_mode_description_subject() {
    return radio_mode_description_subject_.native();
}

lv_subject_t* BaseViewModel::frequency_subject() {
    return frequency_subject_.native();
}

lv_subject_t* BaseViewModel::band_mode_subject() {
    return band_mode_subject_.native();
}

lv_subject_t* BaseViewModel::receiver_settings_subject() {
    return receiver_settings_subject_.native();
}

lv_subject_t* BaseViewModel::signal_level_subject() { return signal_level_subject_.native(); }
lv_subject_t* BaseViewModel::transmit_meter_subject() { return transmit_meter_subject_.native(); }
lv_subject_t* BaseViewModel::radio_detector_subject() { return radio_detector_subject_.native(); }
lv_subject_t* BaseViewModel::radio_pa_subject() { return radio_pa_subject_.native(); }
lv_subject_t* BaseViewModel::radio_inputs_subject() { return radio_inputs_subject_.native(); }
lv_subject_t* BaseViewModel::radio_alert_subject() { return radio_alert_subject_.native(); }

lv_subject_t* BaseViewModel::radio_connection_state_subject() {
    return radio_connection_state_subject_.native();
}

lv_subject_t* BaseViewModel::dark_mode_subject() {
    return dark_mode_subject_.native();
}

lv_subject_t* BaseViewModel::current_page_subject() {
    return current_page_subject_.native();
}

lv_subject_t* BaseViewModel::counter_subject() {
    return counter_subject_.native();
}

lv_subject_t* BaseViewModel::bold_text_subject() {
    return bold_text_subject_.native();
}

lv_subject_t* BaseViewModel::info_visible_subject() {
    return info_visible_subject_.native();
}

lv_subject_t* BaseViewModel::quit_requested_subject() {
    return quit_requested_subject_.native();
}

bool BaseViewModel::is_dark_mode() const {
    return model_.dark_mode();
}

uint64_t BaseViewModel::frequency_hz() const {
    return model_.frequency_hz();
}

model::RadioMode BaseViewModel::radio_mode() const {
    return model_.radio_mode();
}

uint32_t BaseViewModel::filter_bandwidth_hz() const {
    return model_.filter_bandwidth_hz();
}

void BaseViewModel::set_radio_connection_state(model::RadioConnectionState state) {
    lv_subject_set_int(radio_connection_state_subject_.native(), static_cast<int>(state));
}

void BaseViewModel::set_radio_measurements(const model::RadioMeasurements& measurements) {
    model_.set_radio_measurements(measurements);
    const auto& current = model_.radio_measurements();
    char text[64]{};
    if (current.signal_level_dbfs) {
        const int above_s9 = *current.signal_level_dbfs + 56;
        if (above_s9 > 0) {
            std::snprintf(text, sizeof(text), "~S9+%d dB", above_s9);
        } else {
            const int s_unit = std::clamp(9 + above_s9 / 6 - (above_s9 % 6 != 0 ? 1 : 0), 0, 9);
            std::snprintf(text, sizeof(text), "~S%d", s_unit);
        }
    } else {
        std::snprintf(text, sizeof(text), "S --");
    }
    if (std::strcmp(signal_level_subject_.value(), text) != 0) signal_level_subject_.set(text);

    char power[16] = "--";
    char swr[16] = "--";
    if (current.forward_watts) std::snprintf(power, sizeof(power), "~%.1f", *current.forward_watts);
    if (current.swr) {
        if (*current.swr > 99.0f) std::snprintf(swr, sizeof(swr), ">99");
        else std::snprintf(swr, sizeof(swr), "~%.1f", *current.swr);
    }
    std::snprintf(text, sizeof(text), "P %sW  SWR %s:1", power, swr);
    if (std::strcmp(transmit_meter_subject_.value(), text) != 0) transmit_meter_subject_.set(text);

    char forward[8] = "--";
    char reverse[8] = "--";
    char temperature[16] = "--";
    char current_value[16] = "--";
    if (current.forward_power_raw) std::snprintf(forward, sizeof(forward), "%u", *current.forward_power_raw);
    if (current.reverse_power_raw) std::snprintf(reverse, sizeof(reverse), "%u", *current.reverse_power_raw);
    if (current.temperature_celsius) std::snprintf(temperature, sizeof(temperature), "%.1f", *current.temperature_celsius);
    if (current.pa_current_amps) std::snprintf(current_value, sizeof(current_value), "%.2f", *current.pa_current_amps);
    std::snprintf(text, sizeof(text), "FWD %s   REV %s (ADC)", forward, reverse);
    if (std::strcmp(radio_detector_subject_.value(), text) != 0) radio_detector_subject_.set(text);
    std::snprintf(text, sizeof(text), "PA %sC  %sA", temperature, current_value);
    if (std::strcmp(radio_pa_subject_.value(), text) != 0) radio_pa_subject_.set(text);
    std::snprintf(text, sizeof(text), "PTT %s   ADC overload %s",
                  current.ptt ? (*current.ptt ? "ON" : "OFF") : "--",
                  current.adc_overload ? (*current.adc_overload ? "YES" : "NO") : "--");
    if (std::strcmp(radio_inputs_subject_.value(), text) != 0) radio_inputs_subject_.set(text);
    std::snprintf(text, sizeof(text), "%s%s%s",
                  current.ptt && *current.ptt ? "PTT" : "",
                  current.ptt && *current.ptt && current.adc_overload && *current.adc_overload ? " " : "",
                  current.adc_overload && *current.adc_overload ? "ADC CLIP" : "");
    if (std::strcmp(radio_alert_subject_.value(), text) != 0) radio_alert_subject_.set(text);
}

void BaseViewModel::set_dark_mode(bool enabled) {
    model_.set_dark_mode(enabled);
    publish_all();
}

void BaseViewModel::toggle_dark_mode() {
    model_.toggle_dark_mode();
    publish_all();
}

model::AppPage BaseViewModel::current_page() const {
    return model_.current_page();
}

void BaseViewModel::show_apple_page() {
    model_.set_current_page(model::AppPage::Apple);
    publish_all();
}

void BaseViewModel::show_butter_page() {
    model_.set_current_page(model::AppPage::Butter);
    publish_all();
}

void BaseViewModel::toggle_page() {
    model_.toggle_page();
    publish_all();
}

void BaseViewModel::select_next_radio_mode() {
    model_.cycle_radio_mode(1);
    publish_all();
}

void BaseViewModel::select_previous_radio_mode() {
    model_.cycle_radio_mode(-1);
    publish_all();
}

void BaseViewModel::tune_frequency(int direction) {
    model_.tune_frequency(direction);
    publish_all();
}

void BaseViewModel::cycle_band(int direction) {
    model_.cycle_band(direction);
    publish_all();
}

void BaseViewModel::cycle_tuning_step(int direction) {
    model_.cycle_tuning_step(direction);
    publish_all();
}

void BaseViewModel::cycle_filter_bandwidth(int direction) {
    model_.cycle_filter_bandwidth(direction);
    publish_all();
}

void BaseViewModel::increment_counter() {
    counter_subject_.set(counter_subject_.value() + 1);
}

void BaseViewModel::decrement_counter() {
    const auto next = counter_subject_.value() - 1;
    counter_subject_.set(next < 0 ? 0 : next);
}

void BaseViewModel::toggle_bold_text() {
    bold_text_subject_.toggle();
}

void BaseViewModel::toggle_info() {
    info_visible_subject_.toggle();
}

void BaseViewModel::request_quit() {
    quit_requested_subject_.set(true);
}

void BaseViewModel::publish_all() {
    title_subject_.set(model_.app_title());
    greeting_subject_.set(model_.greeting());
    radio_mode_subject_.set(model_.radio_mode_name());
    radio_mode_description_subject_.set(model_.radio_mode_description());
    char text[32]{};
    format_frequency(model_.frequency_hz(), text, sizeof(text));
    frequency_subject_.set(text);
    format_band_mode(model_, text, sizeof(text));
    band_mode_subject_.set(text);
    format_settings(model_, text, sizeof(text));
    receiver_settings_subject_.set(text);
    set_radio_measurements(model_.radio_measurements());
    dark_mode_subject_.set(model_.dark_mode());
    current_page_subject_.set(page_to_int(model_.current_page()));
}

} // namespace viewmodel
