/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace platform::sdr {

struct MetisRadioEndpoint {
    std::string address;
    uint16_t port{1024};
};

std::array<uint8_t, 4> make_metis_discovery_packet() noexcept;

bool discover_metis_radios(uint32_t timeout_ms,
                           std::vector<MetisRadioEndpoint>& radios,
                           std::string& error);

} // namespace platform::sdr