/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_model.h"

#include <array>
#include <cassert>
#include <string>

int main() {
    model::BaseModel model;
    assert(std::string(model.band_name()) == "20m");
    assert(model.frequency_hz() == 14200000);
    assert(model.tuning_step_hz() == 100);
    assert(model.filter_bandwidth_hz() == 2800);

    model.tune_frequency(1);
    assert(model.frequency_hz() == 14200100);
    model.cycle_tuning_step(1);
    assert(model.tuning_step_hz() == 500);

    for (int step = 0; step < 1000; ++step) {
        model.tune_frequency(1);
    }
    assert(model.frequency_hz() == 14700100);
    for (int step = 0; step < 1000; ++step) {
        model.tune_frequency(-1);
    }
    assert(model.frequency_hz() == 14200100);
    for (int step = 0; step < 1000; ++step) {
        model.tune_frequency(-1);
    }
    assert(model.frequency_hz() == 13700100);
    assert(std::string(model.band_name()) == "RX ONLY");

    model::BaseModel edge;
    edge.cycle_tuning_step(1);
    edge.cycle_tuning_step(1);
    edge.cycle_tuning_step(1);
    edge.cycle_tuning_step(1);
    for (int step = 0; step < 29; ++step) assert(!edge.tune_frequency(1));
    assert(edge.tune_frequency(1));
    assert(edge.frequency_hz() == 14350000);
    assert(!edge.tune_frequency(1));
    assert(std::string(edge.band_name()) == "RX ONLY");
    assert(edge.tune_frequency(-1));
    assert(std::string(edge.band_name()) == "20m");

    model::BaseModel sixty;
    sixty.cycle_band(-1);
    sixty.cycle_band(-1);
    sixty.cycle_band(-1);
    for (int step = 0; step < 4; ++step) sixty.cycle_tuning_step(1);
    assert(std::string(sixty.band_name()) == "60m");
    assert(sixty.tune_frequency(-1));
    assert(sixty.frequency_hz() == 5350000);
    assert(std::string(sixty.band_name()) == "RX ONLY");
    assert(sixty.tune_frequency(1));
    assert(std::string(sixty.band_name()) == "60m");

    model::BaseModel limits;
    for (int step = 0; step < 4; ++step) limits.cycle_tuning_step(1);
    for (int step = 0; step < 6000; ++step) limits.tune_frequency(-1);
    assert(limits.frequency_hz() == 0);
    assert(!limits.tune_frequency(-1));
    for (int step = 0; step < 7000; ++step) limits.tune_frequency(1);
    assert(limits.frequency_hz() == 30000000);
    assert(!limits.tune_frequency(1));

    model::BaseModel band_defaults;
    constexpr std::array<model::RadioMode, 10> expected_modes = {
        model::RadioMode::Usb,
        model::RadioMode::Usb,
        model::RadioMode::Usb,
        model::RadioMode::Usb,
        model::RadioMode::Lsb,
        model::RadioMode::Lsb,
        model::RadioMode::Lsb,
        model::RadioMode::Lsb,
        model::RadioMode::Cw,
        model::RadioMode::Usb,
    };
    for (const auto expected_mode : expected_modes) {
        band_defaults.cycle_band(1);
        assert(band_defaults.radio_mode() == expected_mode);
    }

    model.cycle_band(1);
    assert(std::string(model.band_name()) == "17m");
    assert(model.frequency_hz() == 18100000);
    assert(model.radio_mode() == model::RadioMode::Usb);
    model.cycle_band(-1);
    model.cycle_band(-1);
    model.cycle_band(-1);
    model.cycle_band(-1);
    assert(std::string(model.band_name()) == "60m");
    assert(model.frequency_hz() == 5355000);
    model.cycle_band(1);
    model.cycle_band(1);
    model.cycle_band(1);
    assert(std::string(model.band_name()) == "20m");
    assert(model.radio_mode() == model::RadioMode::Usb);

    model.cycle_radio_mode(1);
    assert(model.radio_mode() == model::RadioMode::Lsb);
    model.cycle_radio_mode(1);
    assert(model.radio_mode() == model::RadioMode::Am);
    assert(model.filter_bandwidth_hz() == 4500);
    model.cycle_filter_bandwidth(1);
    assert(model.filter_bandwidth_hz() == 6000);
    model.cycle_radio_mode(1);
    assert(model.radio_mode() == model::RadioMode::Cw);
    assert(model.filter_bandwidth_hz() == 900);
    model.cycle_filter_bandwidth(-1);
    assert(model.filter_bandwidth_hz() == 700);
}