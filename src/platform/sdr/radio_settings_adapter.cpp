/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "radio_settings_adapter.h"

namespace platform::sdr {
namespace {

DemodMode to_demod_mode(model::RadioMode mode) {
    switch (mode) {
    case model::RadioMode::Usb: return DemodMode::Usb;
    case model::RadioMode::Lsb: return DemodMode::Lsb;
    case model::RadioMode::Am: return DemodMode::Am;
    case model::RadioMode::Cw: return DemodMode::Cw;
    }
    return DemodMode::Usb;
}

} // namespace

DemodulatorSettings make_demodulator_settings(const model::BaseModel& model) {
    return make_demodulator_settings(model.radio_mode(), model.filter_bandwidth_hz());
}

DemodulatorSettings make_demodulator_settings(model::RadioMode mode, uint32_t bandwidth_hz) {
    DemodulatorSettings settings;
    settings.mode = to_demod_mode(mode);
    settings.audio_bandwidth_hz = bandwidth_hz;
    return settings;
}

} // namespace platform::sdr