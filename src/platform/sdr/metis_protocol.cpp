/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "metis_protocol.h"

#include <algorithm>
#include <cmath>

namespace platform::sdr {
namespace {

constexpr size_t kHeaderSize = 8;
constexpr size_t kBlockSize = 512;
constexpr size_t kBlockHeaderSize = 8;
constexpr size_t kBlocksPerPacket = 2;
constexpr size_t kIqSampleSize = 6;
constexpr size_t kAudioSampleSize = 2;
constexpr size_t kSamplesPerBlock =
    (kBlockSize - kBlockHeaderSize) / (kIqSampleSize + kAudioSampleSize);

int32_t decode_signed_24(const uint8_t* bytes) noexcept {
    uint32_t value = (static_cast<uint32_t>(bytes[0]) << 16)
                   | (static_cast<uint32_t>(bytes[1]) << 8)
                   | static_cast<uint32_t>(bytes[2]);
    if ((value & 0x800000U) != 0) {
        return static_cast<int32_t>(value) - 0x1000000;
    }
    return static_cast<int32_t>(value);
}

} // namespace

void MetisTelemetry::update(const MetisIqPacket& packet) noexcept {
    for (size_t offset = 0; offset < packet.control.size(); offset += 5) {
        const auto* control = packet.control.data() + offset;
        ptt = (control[0] & 0x01U) != 0;
        if ((control[0] & 0x80U) != 0) {
            continue;
        }
        const auto first = static_cast<uint16_t>((static_cast<uint16_t>(control[1]) << 8) | control[2]);
        const auto second = static_cast<uint16_t>((static_cast<uint16_t>(control[3]) << 8) | control[4]);
        switch ((control[0] >> 3) & 0x0fU) {
        case 0:
            adc_overload = (control[1] & 0x01U) != 0;
            break;
        case 1:
            temperature_raw = first;
            forward_power_raw = second;
            break;
        case 2:
            reverse_power_raw = first;
            pa_current_raw = second;
            break;
        default:
            break;
        }
    }
}

MetisPowerEstimate estimate_metis_power(const MetisTelemetry& telemetry) noexcept {
    MetisPowerEstimate estimate;
    if (!telemetry.forward_power_raw || *telemetry.forward_power_raw > 4095) return estimate;
    const auto detector_volts = [](uint16_t counts) {
        return static_cast<float>(std::max(0, static_cast<int>(counts) - 6)) * (3.3f / 4095.0f);
    };
    const float forward_voltage = detector_volts(*telemetry.forward_power_raw);
    estimate.forward_watts = forward_voltage * forward_voltage / 1.5f;
    if (*estimate.forward_watts <= 2.0f || !telemetry.reverse_power_raw
        || *telemetry.reverse_power_raw > 4095) return estimate;
    const float reverse_voltage = detector_volts(*telemetry.reverse_power_raw);
    if (reverse_voltage >= forward_voltage) return estimate;
    const float reflection = reverse_voltage / forward_voltage;
    estimate.swr = (1.0f + reflection) / (1.0f - reflection);
    return estimate;
}

std::optional<float> metis_temperature_celsius(std::optional<uint16_t> raw) noexcept {
    if (!raw || *raw == 0 || *raw > 4095) return std::nullopt;
    return (3.26f * static_cast<float>(*raw) / 4096.0f - 0.5f) / 0.01f;
}

std::optional<float> metis_pa_current_amps(std::optional<uint16_t> raw) noexcept {
    if (!raw || *raw > 4095) return std::nullopt;
    return static_cast<float>(*raw) * (3.26f / 4096.0f) * 1.27f / (50.0f * 0.04f);
}

MetisPacketError parse_metis_ep6_packet(const uint8_t* data,
                                        size_t size,
                                        MetisIqPacket& packet) noexcept {
    packet = {};
    if (!data) {
        return MetisPacketError::NullPacket;
    }
    if (size != kMetisEp6PacketSize) {
        return MetisPacketError::IncorrectSize;
    }
    if (data[0] != 0xEF || data[1] != 0xFE) {
        return MetisPacketError::IncorrectMagic;
    }
    if (data[2] != 0x01) {
        return MetisPacketError::UnsupportedPacketType;
    }
    if ((data[3] & 0x7F) != 0x06) {
        return MetisPacketError::UnexpectedEndpoint;
    }

    packet.sequence = (static_cast<uint32_t>(data[4]) << 24)
                    | (static_cast<uint32_t>(data[5]) << 16)
                    | (static_cast<uint32_t>(data[6]) << 8)
                    | static_cast<uint32_t>(data[7]);

    for (size_t block_index = 0; block_index < kBlocksPerPacket; ++block_index) {
        const size_t block_offset = kHeaderSize + block_index * kBlockSize;
        const auto* block = data + block_offset;
        if (block[0] != 0x7F || block[1] != 0x7F || block[2] != 0x7F) {
            packet = {};
            return MetisPacketError::InvalidSync;
        }

        for (size_t index = 0; index < 5; ++index) {
            packet.control[block_index * 5 + index] = block[3 + index];
        }

        for (size_t sample_index = 0; sample_index < kSamplesPerBlock; ++sample_index) {
            const size_t sample_offset = kBlockHeaderSize
                                       + sample_index * (kIqSampleSize + kAudioSampleSize);
            auto& sample = packet.samples[packet.sample_count++];
            sample.i = decode_signed_24(block + sample_offset);
            sample.q = decode_signed_24(block + sample_offset + 3);
        }
    }

    return MetisPacketError::None;
}

} // namespace platform::sdr