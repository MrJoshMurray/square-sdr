/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

#include <array>
#include <cstdint>

namespace screen {

class ReceiverScreen : public BaseScreen {
public:
    ReceiverScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~ReceiverScreen() override;

private:
    static void spectrum_timer_cb(lv_timer_t* timer);
    void update_spectrum();
    void build_content(lv_obj_t* content) override;

    static constexpr int32_t kCanvasWidth = 288;
    static constexpr int32_t kSpectrumHeight = 20;
    static constexpr int32_t kWaterfallHeight = 24;

    lv_obj_t* spectrum_canvas_{nullptr};
    lv_obj_t* waterfall_canvas_{nullptr};
    lv_timer_t* spectrum_timer_{nullptr};
    std::array<uint16_t, kCanvasWidth * kSpectrumHeight> spectrum_pixels_{};
    std::array<uint16_t, kCanvasWidth * kWaterfallHeight> waterfall_pixels_{};
    uint32_t frame_index_{0};
    uint64_t spectrum_sequence_{0};
};

} // namespace screen