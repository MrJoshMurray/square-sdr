/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdr/metis_discovery.h"

#include <iostream>
#include <string>
#include <vector>

int main() {
    std::vector<platform::sdr::MetisRadioEndpoint> radios;
    std::string error;
    if (!platform::sdr::discover_metis_radios(1500, radios, error)) {
        std::cerr << "Metis discovery failed: " << error << '\n';
        return 1;
    }

    if (radios.empty()) {
        std::cout << "No Metis radios replied to discovery.\n";
        return 2;
    }
    for (const auto& radio : radios) {
        std::cout << radio.address << ':' << radio.port << '\n';
    }
    return 0;
}