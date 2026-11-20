/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/metis_stream.h"

#include <array>
#include <cassert>

int main() {
    const auto start = platform::sdr::make_metis_iq_stream_command(true);
    const auto stop = platform::sdr::make_metis_iq_stream_command(false);
    assert(start[0] == 0xEF && start[1] == 0xFE && start[2] == 0x04 && start[3] == 0x01);
    assert(stop[0] == 0xEF && stop[1] == 0xFE && stop[2] == 0x04 && stop[3] == 0x00);
    for (size_t index = 4; index < start.size(); ++index) {
        assert(start[index] == 0);
        assert(stop[index] == 0);
    }
    const auto config = platform::sdr::make_metis_receiver_config_packet(14200000);
    assert(config[0] == 0xEF && config[1] == 0xFE
        && config[2] == 0x01 && config[3] == 0x02);
    for (size_t block = 0; block < 2; ++block) {
        const size_t offset = 8 + block * 512;
        if (block == 0) {
            assert(config[offset + 3] == 0x00);
            assert(config[offset + 4] == 0xE4);
            assert(config[offset + 5] == 0x90);
            assert(config[offset + 6] == 0x04);
            assert(config[offset + 7] == 0x04);
            assert((config[offset + 7] & 0x38) == 0);
        }
        else {
            assert(config[offset + 3] == 0x04);
            assert(config[offset + 4] == 0x00 && config[offset + 5] == 0xD8
                && config[offset + 6] == 0xAC && config[offset + 7] == 0xC0);
        }
    }

    struct FilterCase {
        uint32_t frequency_hz;
        uint8_t expected_control;
    };
    constexpr std::array<FilterCase, 24> filter_cases = {{
        {1599999, 0x00}, {1600000, 0x02}, {2499999, 0x02},
        {2500000, 0x84}, {4499999, 0x84}, {4500000, 0x88},
        {8499999, 0x88}, {8500000, 0x90}, {16499999, 0x90},
        {16500000, 0xA0}, {22499999, 0xA0}, {22500000, 0xC0},
        {30000000, 0xC0}, {30000001, 0x00},
        {5350000, 0x88}, {7100000, 0x88},
        {10125000, 0x90}, {14200000, 0x90},
        {1900000, 0x02}, {3700000, 0x84},
        {18100000, 0xA0}, {21000000, 0xA0},
        {24940000, 0xC0}, {28500000, 0xC0},
    }};
    for (const auto& test : filter_cases) {
        const auto packet = platform::sdr::make_metis_receiver_config_packet(test.frequency_hz);
        assert(packet[11] == 0x00);
        assert(packet[12] == 0xE4);
        assert(packet[13] == test.expected_control);
        assert((packet[13] & 0x01) == 0);
        assert(packet[14] == 0x04 && packet[15] == 0x04);
        assert(packet[523] == 0x04);
        assert((packet[523] & 0x01) == 0);
        assert(packet[524] == static_cast<uint8_t>(test.frequency_hz >> 24));
        assert(packet[525] == static_cast<uint8_t>(test.frequency_hz >> 16));
        assert(packet[526] == static_cast<uint8_t>(test.frequency_hz >> 8));
        assert(packet[527] == static_cast<uint8_t>(test.frequency_hz));
        for (size_t block = 0; block < 2; ++block) {
            const size_t payload_start = 8 + block * 512 + 8;
            for (size_t index = payload_start; index < 8 + (block + 1) * 512; ++index) {
                assert(packet[index] == 0);
            }
        }
    }
    return 0;
}