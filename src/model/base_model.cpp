/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_model.h"

#include <algorithm>
#include <array>

namespace model {

const RadioMeasurements& BaseModel::radio_measurements() const {
    return radio_measurements_;
}

void BaseModel::set_radio_measurements(const RadioMeasurements& measurements) {
    radio_measurements_ = measurements;
}
namespace {

struct BandPlanEntry {
    const char* name;
    uint64_t low_hz;
    uint64_t high_hz;
    uint64_t default_hz;
};

constexpr std::array<BandPlanEntry, 10> kUkHfBands = {{
    {"160m", 1810000, 2000000, 1900000},
    {"80m", 3500000, 3800000, 3700000},
    {"60m", 5258500, 5406500, 5355000},
    {"40m", 7000000, 7200000, 7100000},
    {"30m", 10100000, 10150000, 10125000},
    {"20m", 14000000, 14350000, 14200000},
    {"17m", 18068000, 18168000, 18100000},
    {"15m", 21000000, 21450000, 21200000},
    {"12m", 24890000, 24990000, 24940000},
    {"10m", 28000000, 29700000, 28500000},
}};

constexpr std::array<BandPlanEntry, 22> kUkAllocations = {{
    {"2200m", 135700, 137800, 0},
    {"630m", 472000, 479000, 0},
    kUkHfBands[0], kUkHfBands[1],
    {"60m", 5258500, 5264000, 0},
    {"60m", 5276000, 5284000, 0},
    {"60m", 5288500, 5292000, 0},
    {"60m", 5298000, 5307000, 0},
    {"60m", 5313000, 5323000, 0},
    {"60m", 5333000, 5338000, 0},
    {"60m", 5354000, 5358000, 0},
    {"60m", 5362000, 5374500, 0},
    {"60m", 5378000, 5382000, 0},
    {"60m", 5395000, 5401500, 0},
    {"60m", 5403500, 5406500, 0},
    kUkHfBands[3], kUkHfBands[4], kUkHfBands[5], kUkHfBands[6],
    kUkHfBands[7], kUkHfBands[8], kUkHfBands[9],
}};

constexpr std::array<RadioMode, 10> kDefaultBandModes = {
    RadioMode::Lsb,
    RadioMode::Lsb,
    RadioMode::Lsb,
    RadioMode::Lsb,
    RadioMode::Cw,
    RadioMode::Usb,
    RadioMode::Usb,
    RadioMode::Usb,
    RadioMode::Usb,
    RadioMode::Usb,
};

constexpr std::array<uint32_t, 7> kTuningStepsHz = {
    10, 50, 100, 500, 1000, 2500, 5000,
};

constexpr std::array<uint32_t, 6> kSsbBandwidthsHz = {
    1800, 2100, 2400, 2700, 2800, 3000,
};

constexpr std::array<uint32_t, 5> kAmBandwidthsHz = {
    3000, 4000, 4500, 6000, 8000,
};

constexpr std::array<uint32_t, 6> kCwBandwidthsHz = {
    100, 250, 500, 700, 900, 1200,
};

uint32_t default_bandwidth_hz(RadioMode mode) {
    switch (mode) {
    case RadioMode::Usb:
    case RadioMode::Lsb:
        return 2800;
    case RadioMode::Am:
        return 4500;
    case RadioMode::Cw:
        return 900;
    }
    return 2800;
}

template <size_t Size>
uint32_t cycle_value(uint32_t current, const std::array<uint32_t, Size>& values, int direction) {
    size_t index = 0;
    for (size_t candidate = 0; candidate < values.size(); ++candidate) {
        if (values[candidate] == current) {
            index = candidate;
            break;
        }
    }
    index = direction < 0 ? (index + values.size() - 1) % values.size()
                          : (index + 1) % values.size();
    return values[index];
}

} // namespace

const char* BaseModel::app_title() const {
    return "Square SDR";
}

const char* BaseModel::greeting() const {
    return "Radio unavailable";
}

const char* BaseModel::radio_mode_name() const {
    switch (radio_mode_) {
    case RadioMode::Usb: return "USB";
    case RadioMode::Lsb: return "LSB";
    case RadioMode::Am: return "AM";
    case RadioMode::Cw: return "CW";
    }
    return "USB";
}

const char* BaseModel::radio_mode_description() const {
    switch (radio_mode_) {
    case RadioMode::Usb: return "Upper sideband";
    case RadioMode::Lsb: return "Lower sideband";
    case RadioMode::Am: return "Amplitude modulation";
    case RadioMode::Cw: return "Morse / 700 Hz tone";
    }
    return "Upper sideband";
}

const char* BaseModel::band_name() const {
    for (const auto& band : kUkAllocations) {
        if (frequency_hz_ >= band.low_hz && frequency_hz_ <= band.high_hz) {
            return band.name;
        }
    }
    return "RX ONLY";
}

uint64_t BaseModel::frequency_hz() const {
    return frequency_hz_;
}

uint32_t BaseModel::tuning_step_hz() const {
    return kTuningStepsHz[tuning_step_index_];
}

uint32_t BaseModel::filter_bandwidth_hz() const {
    return filter_bandwidth_hz_;
}

bool BaseModel::dark_mode() const {
    return dark_mode_;
}

void BaseModel::set_dark_mode(bool enabled) {
    dark_mode_ = enabled;
}

void BaseModel::toggle_dark_mode() {
    dark_mode_ = !dark_mode_;
}

AppPage BaseModel::current_page() const {
    return current_page_;
}


void BaseModel::set_current_page(AppPage page) {
    current_page_ = page;
}

void BaseModel::toggle_page() {
    current_page_ = current_page_ == AppPage::Apple ? AppPage::Butter : AppPage::Apple;
}

RadioMode BaseModel::radio_mode() const {
    return radio_mode_;
}

void BaseModel::cycle_radio_mode(int direction) {
    if (direction == 0) {
        return;
    }
    constexpr int kModeCount = 4;
    const int current = static_cast<int>(radio_mode_);
    const int step = direction < 0 ? kModeCount - 1 : 1;
    radio_mode_ = static_cast<RadioMode>((current + step) % kModeCount);
    filter_bandwidth_hz_ = default_bandwidth_hz(radio_mode_);
}

bool BaseModel::tune_frequency(int direction) {
    if (direction == 0) {
        return false;
    }
    const uint64_t previous_hz = frequency_hz_;
    if (direction < 0) {
        frequency_hz_ = frequency_hz_ > tuning_step_hz()
                      ? frequency_hz_ - tuning_step_hz()
                      : 0;
    }
    else {
        frequency_hz_ = std::min<uint64_t>(frequency_hz_ + tuning_step_hz(), 30000000);
    }
    for (const auto& band : kUkAllocations) {
        for (const auto edge : {band.low_hz, band.high_hz}) {
            if ((previous_hz < edge && frequency_hz_ >= edge)
                || (previous_hz > edge && frequency_hz_ <= edge)) {
                return true;
            }
        }
    }
    return false;
}

void BaseModel::cycle_band(int direction) {
    if (direction == 0) {
        return;
    }
    constexpr size_t kBandCount = kUkHfBands.size();
    band_index_ = static_cast<uint8_t>(direction < 0
                 ? (band_index_ + kBandCount - 1) % kBandCount
                 : (band_index_ + 1) % kBandCount);
    frequency_hz_ = kUkHfBands[band_index_].default_hz;
    radio_mode_ = kDefaultBandModes[band_index_];
    filter_bandwidth_hz_ = default_bandwidth_hz(radio_mode_);
}

void BaseModel::cycle_tuning_step(int direction) {
    if (direction == 0) {
        return;
    }
    tuning_step_index_ = static_cast<uint8_t>(direction < 0
                         ? (tuning_step_index_ + kTuningStepsHz.size() - 1) % kTuningStepsHz.size()
                         : (tuning_step_index_ + 1) % kTuningStepsHz.size());
}

void BaseModel::cycle_filter_bandwidth(int direction) {
    if (direction == 0) {
        return;
    }
    switch (radio_mode_) {
    case RadioMode::Usb:
    case RadioMode::Lsb:
        filter_bandwidth_hz_ = cycle_value(filter_bandwidth_hz_, kSsbBandwidthsHz, direction);
        break;
    case RadioMode::Am:
        filter_bandwidth_hz_ = cycle_value(filter_bandwidth_hz_, kAmBandwidthsHz, direction);
        break;
    case RadioMode::Cw:
        filter_bandwidth_hz_ = cycle_value(filter_bandwidth_hz_, kCwBandwidthsHz, direction);
        break;
    }
}

} // namespace model
