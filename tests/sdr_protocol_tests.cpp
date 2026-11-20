/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/metis_protocol.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace {

constexpr size_t kBlockSize = 512;
constexpr size_t kBlockHeaderSize = 8;
constexpr size_t kSampleSize = 8;

void encode_signed_24(uint8_t* destination, int32_t value) {
    const auto encoded = static_cast<uint32_t>(value);
    destination[0] = static_cast<uint8_t>(encoded >> 16);
    destination[1] = static_cast<uint8_t>(encoded >> 8);
    destination[2] = static_cast<uint8_t>(encoded);
}

std::array<uint8_t, platform::sdr::kMetisEp6PacketSize> make_packet() {
    std::array<uint8_t, platform::sdr::kMetisEp6PacketSize> packet{};
    packet[0] = 0xEF;
    packet[1] = 0xFE;
    packet[2] = 0x01;
    packet[3] = 0x06;
    packet[4] = 0x12;
    packet[5] = 0x34;
    packet[6] = 0x56;
    packet[7] = 0x78;

    for (size_t block_index = 0; block_index < 2; ++block_index) {
        const size_t block_offset = 8 + block_index * kBlockSize;
        packet[block_offset] = 0x7F;
        packet[block_offset + 1] = 0x7F;
        packet[block_offset + 2] = 0x7F;
        packet[block_offset + 3] = static_cast<uint8_t>(block_index);

        for (size_t sample_index = 0; sample_index < 63; ++sample_index) {
            auto* sample = packet.data() + block_offset + kBlockHeaderSize
                         + sample_index * kSampleSize;
            encode_signed_24(sample, block_index == 0 && sample_index == 0 ? -8388608 : 123456);
            encode_signed_24(sample + 3, block_index == 0 && sample_index == 0 ? -1 : -654321);
        }
    }

    return packet;
}

} // namespace

int main() {
    using platform::sdr::MetisPacketError;
    using platform::sdr::MetisIqPacket;
    using platform::sdr::parse_metis_ep6_packet;

    auto bytes = make_packet();
    MetisIqPacket parsed;
    assert(parse_metis_ep6_packet(bytes.data(), bytes.size(), parsed) == MetisPacketError::None);
    assert(parsed.sequence == 0x12345678U);
    assert(parsed.sample_count == platform::sdr::kMetisIqSamplesPerPacket);
    assert(parsed.control[0] == 0);
    assert(parsed.control[5] == 1);
    assert(parsed.samples[0].i == -8388608);
    assert(parsed.samples[0].q == -1);
    assert(parsed.samples[63].i == 123456);
    assert(parsed.samples[63].q == -654321);

    platform::sdr::MetisTelemetry telemetry;
    assert(!telemetry.forward_power_raw.has_value());
    parsed.control = {0x09, 0x01, 0x23, 0x04, 0x56,
                      0x10, 0x07, 0x89, 0x00, 0xab};
    telemetry.update(parsed);
    assert(telemetry.ptt == false);
    assert(telemetry.temperature_raw == 0x123);
    assert(telemetry.forward_power_raw == 0x456);

    platform::sdr::MetisTelemetry power_telemetry;
    power_telemetry.forward_power_raw = 3383;
    power_telemetry.reverse_power_raw = 6;
    auto estimate = platform::sdr::estimate_metis_power(power_telemetry);
    assert(estimate.forward_watts && std::abs(*estimate.forward_watts - 4.94f) < 0.02f);
    assert(estimate.swr && std::abs(*estimate.swr - 1.0f) < 0.001f);
    power_telemetry.reverse_power_raw = 1132;
    estimate = platform::sdr::estimate_metis_power(power_telemetry);
    assert(estimate.swr && std::abs(*estimate.swr - 2.0f) < 0.01f);
    power_telemetry.reverse_power_raw = 3383;
    assert(!platform::sdr::estimate_metis_power(power_telemetry).swr);
    power_telemetry.forward_power_raw = 100;
    assert(!platform::sdr::estimate_metis_power(power_telemetry).swr);
    power_telemetry.forward_power_raw = 4096;
    assert(!platform::sdr::estimate_metis_power(power_telemetry).forward_watts);
    assert(!platform::sdr::metis_temperature_celsius(std::nullopt));
    assert(!platform::sdr::metis_temperature_celsius(0));
    assert(!platform::sdr::metis_temperature_celsius(4096));
    const auto freezing = platform::sdr::metis_temperature_celsius(628);
    const auto room = platform::sdr::metis_temperature_celsius(942);
    const auto warm = platform::sdr::metis_temperature_celsius(1256);
    assert(freezing && std::abs(*freezing) < 0.1f);
    assert(room && std::abs(*room - 25.0f) < 0.1f);
    assert(warm && std::abs(*warm - 50.0f) < 0.1f);
    assert(!platform::sdr::metis_pa_current_amps(std::nullopt));
    assert(!platform::sdr::metis_pa_current_amps(4096));
    const auto no_current = platform::sdr::metis_pa_current_amps(0);
    const auto one_amp = platform::sdr::metis_pa_current_amps(1978);
    assert(no_current && *no_current == 0.0f);
    assert(one_amp && std::abs(*one_amp - 1.0f) < 0.001f);
    assert(telemetry.reverse_power_raw == 0x789);
    assert(telemetry.pa_current_raw == 0xab);
    parsed.control = {0x81, 0xff, 0xff, 0xff, 0xff,
                      0x01, 0x01, 0x00, 0x00, 0x00};
    telemetry.update(parsed);
    assert(telemetry.ptt == true);
    assert(telemetry.adc_overload == true);
    assert(telemetry.temperature_raw == 0x123);
    assert(telemetry.forward_power_raw == 0x456);

    assert(parse_metis_ep6_packet(nullptr, bytes.size(), parsed) == MetisPacketError::NullPacket);
    assert(parse_metis_ep6_packet(bytes.data(), bytes.size() - 1, parsed) == MetisPacketError::IncorrectSize);

    bytes[0] = 0;
    assert(parse_metis_ep6_packet(bytes.data(), bytes.size(), parsed) == MetisPacketError::IncorrectMagic);
    bytes[0] = 0xEF;
    bytes[2] = 0x02;
    assert(parse_metis_ep6_packet(bytes.data(), bytes.size(), parsed) == MetisPacketError::UnsupportedPacketType);
    bytes[2] = 0x01;
    bytes[3] = 0x04;
    assert(parse_metis_ep6_packet(bytes.data(), bytes.size(), parsed) == MetisPacketError::UnexpectedEndpoint);
    bytes[3] = 0x06;
    bytes[8] = 0;
    assert(parse_metis_ep6_packet(bytes.data(), bytes.size(), parsed) == MetisPacketError::InvalidSync);
}