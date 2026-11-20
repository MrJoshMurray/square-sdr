/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace platform::sdr {

constexpr size_t kMetisEp6PacketSize = 1032;
constexpr size_t kMetisIqSamplesPerPacket = 126;
constexpr uint32_t kDefaultRxSampleRateHz = 48000;

struct IqSample {
    int32_t i{0};
    int32_t q{0};
};

struct MetisIqPacket {
    uint32_t sequence{0};
    std::array<uint8_t, 10> control{};
    std::array<IqSample, kMetisIqSamplesPerPacket> samples{};
    size_t sample_count{0};
};

struct MetisTelemetry {
    std::optional<bool> ptt;
    std::optional<bool> adc_overload;
    std::optional<uint16_t> temperature_raw;
    std::optional<uint16_t> forward_power_raw;
    std::optional<uint16_t> reverse_power_raw;
    std::optional<uint16_t> pa_current_raw;

    void update(const MetisIqPacket& packet) noexcept;
};

struct MetisPowerEstimate {
    std::optional<float> forward_watts;
    std::optional<float> swr;
};

MetisPowerEstimate estimate_metis_power(const MetisTelemetry& telemetry) noexcept;
std::optional<float> metis_temperature_celsius(std::optional<uint16_t> raw) noexcept;
std::optional<float> metis_pa_current_amps(std::optional<uint16_t> raw) noexcept;

enum class MetisPacketError {
    None,
    NullPacket,
    IncorrectSize,
    IncorrectMagic,
    UnsupportedPacketType,
    UnexpectedEndpoint,
    InvalidSync,
};

MetisPacketError parse_metis_ep6_packet(const uint8_t* data,
                                       size_t size,
                                       MetisIqPacket& packet) noexcept;

} // namespace platform::sdr