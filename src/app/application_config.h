/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>

namespace app {

struct ApplicationConfig {
    bool dark_mode = true;
    int tx_drive = 15;
    int mic_gain = 60;
    int sidetone_hz = 700;
    int tx_low_cut_hz = 300;
    int ptt_key = 0; // 0 = Tab, 1 = Space
};

bool load_application_config(const std::string& path,
                             ApplicationConfig& config,
                             std::string& error);
bool save_application_config(const std::string& path,
                             const ApplicationConfig& config,
                             std::string& error);

} // namespace app
