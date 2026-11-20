/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "metis_protocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace platform::sdr {

enum class DemodMode {
    Usb,
    Lsb,
    Am,
    Cw,
};

struct DemodulatorSettings {
    DemodMode mode{DemodMode::Usb};
    uint32_t sample_rate_hz{kDefaultRxSampleRateHz};
    uint32_t cw_pitch_hz{700};
    uint32_t audio_bandwidth_hz{0};
};

class ReceiveDemodulator {
public:
    ReceiveDemodulator();
    ~ReceiveDemodulator();

    ReceiveDemodulator(const ReceiveDemodulator&) = delete;
    ReceiveDemodulator& operator=(const ReceiveDemodulator&) = delete;

    bool configure(const DemodulatorSettings& settings, std::string& error);
    bool process(const IqSample* input,
                 size_t sample_count,
                 float* mono_output,
                 size_t output_capacity,
                 std::string& error);
    std::optional<float> signal_level_dbfs() const;
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform::sdr