/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/metis_discovery.h"

#include <array>
#include <cassert>

int main() {
    constexpr std::array<uint8_t, 4> expected{0xEF, 0xFE, 0x02, 0x00};
    assert(platform::sdr::make_metis_discovery_packet() == expected);
    return 0;
}