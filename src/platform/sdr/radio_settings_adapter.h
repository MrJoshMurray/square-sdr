/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "receive_demodulator.h"

#include "base_model.h"

namespace platform::sdr {

DemodulatorSettings make_demodulator_settings(const model::BaseModel& model);
DemodulatorSettings make_demodulator_settings(model::RadioMode mode, uint32_t bandwidth_hz);

} // namespace platform::sdr