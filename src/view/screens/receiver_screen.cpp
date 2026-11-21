/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "receiver_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "sdr/spectrum_simulator.h"
#include "theme.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace screen {
namespace {

constexpr uint16_t kBackground = 0x0000;
constexpr uint16_t kPassbandColor = 0x3186;

uint16_t heat_color(uint8_t level) {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    if (level < 64) {
        blue = static_cast<uint8_t>(24 + level * 3);
    }
    else if (level < 128) {
        green = static_cast<uint8_t>((level - 64) * 4);
        blue = 255;
    }
    else if (level < 192) {
        red = static_cast<uint8_t>((level - 128) * 4);
        green = 255;
        blue = static_cast<uint8_t>(255 - (level - 128) * 4);
    }
    else {
        red = 255;
        green = static_cast<uint8_t>(255 - std::min<int>(255, (level - 192) * 4));
    }

    return static_cast<uint16_t>(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

void set_label_font(lv_obj_t* label, app::AssetManager& assets, uint16_t size) {
    auto* font = assets.load_standard_font(size, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(label,
                               font ? font : (size <= 10 ? &lv_font_montserrat_10
                                                : size <= 12 ? &lv_font_montserrat_12 : &lv_font_montserrat_20),
                               0);
}

lv_obj_t* create_label(lv_obj_t* parent,
                       app::AssetManager& assets,
                       lv_subject_t* dark_mode,
                       const char* text,
                       uint16_t font_size,
                       int32_t width,
                       int32_t x,
                       int32_t y,
                       lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_size(label, width, LV_SIZE_CONTENT);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_align(label, align, 0);
    set_label_font(label, assets, font_size);
    reactive::bind_theme(label, dark_mode, reactive::ThemeRole::Text);
    return label;
}

} // namespace

ReceiverScreen::ReceiverScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

ReceiverScreen::~ReceiverScreen() {
    if (spectrum_timer_) {
        lv_timer_delete(spectrum_timer_);
        spectrum_timer_ = nullptr;
    }
}

void ReceiverScreen::build_content(lv_obj_t* content) {
    auto* band_mode = lv_label_create(content);
    lv_label_bind_text(band_mode, view_model().band_mode_subject(), nullptr);
    lv_obj_set_size(band_mode, 80, LV_SIZE_CONTENT);
    lv_obj_set_pos(band_mode, 16, 0);
    set_label_font(band_mode, assets(), 12);
    reactive::bind_theme(band_mode, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    auto* signal_level = lv_label_create(content);
    lv_label_bind_text(signal_level, view_model().signal_level_subject(), nullptr);
    lv_obj_set_size(signal_level, 120, LV_SIZE_CONTENT);
    lv_obj_set_pos(signal_level, 184, 0);
    lv_obj_set_style_text_align(signal_level, LV_TEXT_ALIGN_RIGHT, 0);
    set_label_font(signal_level, assets(), 12);
    reactive::bind_theme(signal_level, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    auto* alert = lv_label_create(content);
    lv_label_bind_text(alert, view_model().radio_alert_subject(), nullptr);
    lv_obj_set_size(alert, 88, LV_SIZE_CONTENT);
    lv_obj_set_pos(alert, 96, 0);
    set_label_font(alert, assets(), 10);
    reactive::bind_theme(alert, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    auto* pa = lv_label_create(content);
    lv_label_bind_text(pa, view_model().radio_pa_subject(), nullptr);
    lv_obj_set_size(pa, 120, LV_SIZE_CONTENT);
    lv_obj_set_pos(pa, 184, 16);
    lv_obj_set_style_text_align(pa, LV_TEXT_ALIGN_RIGHT, 0);
    set_label_font(pa, assets(), 10);
    reactive::bind_theme(pa, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    auto* transmit_meter = lv_label_create(content);
    lv_label_bind_text(transmit_meter, view_model().transmit_meter_subject(), nullptr);
    lv_obj_set_size(transmit_meter, 120, LV_SIZE_CONTENT);
    lv_obj_set_pos(transmit_meter, 184, 30);
    lv_obj_set_style_text_align(transmit_meter, LV_TEXT_ALIGN_RIGHT, 0);
    set_label_font(transmit_meter, assets(), 10);
    reactive::bind_theme(transmit_meter, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    auto* frequency = lv_label_create(content);
    lv_label_bind_text(frequency, view_model().frequency_subject(), nullptr);
    lv_obj_set_size(frequency, 160, LV_SIZE_CONTENT);
    lv_obj_set_pos(frequency, 16, 12);
    lv_obj_set_style_text_align(frequency, LV_TEXT_ALIGN_CENTER, 0);
    set_label_font(frequency, assets(), 20);
    reactive::bind_theme(frequency, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    auto* settings = lv_label_create(content);
    lv_label_bind_text(settings, view_model().receiver_settings_subject(), nullptr);
    lv_obj_set_size(settings, 160, LV_SIZE_CONTENT);
    lv_obj_set_pos(settings, 16, 35);
    set_label_font(settings, assets(), 10);
    reactive::bind_theme(settings, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    spectrum_canvas_ = lv_canvas_create(content);
    lv_canvas_set_buffer(spectrum_canvas_,
                         spectrum_pixels_.data(),
                         kCanvasWidth,
                         kSpectrumHeight,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(spectrum_canvas_, 16, 49);

    auto* spectrum_separator = lv_obj_create(content);
    lv_obj_remove_style_all(spectrum_separator);
    lv_obj_set_size(spectrum_separator, kCanvasWidth, 1);
    lv_obj_set_pos(spectrum_separator, 16, 69);
    lv_obj_set_style_bg_color(spectrum_separator, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(spectrum_separator, LV_OPA_COVER, 0);
    lv_obj_clear_flag(spectrum_separator, LV_OBJ_FLAG_CLICKABLE);

    waterfall_canvas_ = lv_canvas_create(content);
    lv_canvas_set_buffer(waterfall_canvas_,
                         waterfall_pixels_.data(),
                         kCanvasWidth,
                         kWaterfallHeight,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(waterfall_canvas_, 16, 70);

    auto* center_marker = lv_obj_create(content);
    lv_obj_remove_style_all(center_marker);
    lv_obj_set_size(center_marker, 1, kWaterfallHeight);
    lv_obj_set_pos(center_marker, 159, 70);
    lv_obj_set_style_bg_color(center_marker, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(center_marker, LV_OPA_80, 0);
    lv_obj_clear_flag(center_marker, LV_OBJ_FLAG_CLICKABLE);

    auto* spectrum_marker = lv_obj_create(content);
    lv_obj_remove_style_all(spectrum_marker);
    lv_obj_set_size(spectrum_marker, 1, kSpectrumHeight);
    lv_obj_set_pos(spectrum_marker, 159, 49);
    lv_obj_set_style_bg_color(spectrum_marker, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(spectrum_marker, LV_OPA_80, 0);
    lv_obj_clear_flag(spectrum_marker, LV_OBJ_FLAG_CLICKABLE);

    create_label(content,
                 assets(),
                 view_model().dark_mode_subject(),
                 "-24 kHz",
                 12,
                 72,
                 16,
                 94);
    create_label(content,
                 assets(),
                 view_model().dark_mode_subject(),
                 "0",
                 12,
                 48,
                 136,
                 94,
                 LV_TEXT_ALIGN_CENTER);
    create_label(content,
                 assets(),
                 view_model().dark_mode_subject(),
                 "+24 kHz",
                 12,
                 72,
                 232,
                 94,
                 LV_TEXT_ALIGN_RIGHT);

    spectrum_timer_ = lv_timer_create(spectrum_timer_cb, 120, this);
    update_spectrum();
}

void ReceiverScreen::spectrum_timer_cb(lv_timer_t* timer) {
    auto* screen = static_cast<ReceiverScreen*>(lv_timer_get_user_data(timer));
    if (screen) {
        screen->update_spectrum();
    }
}

void ReceiverScreen::update_spectrum() {
    platform::sdr::SpectrumFrame frame{};
    if (!platform::sdr::LiveSpectrumBuffer::snapshot(frame)) {
        if (!std::getenv("SQUARE_SDR_LIVE_RADIO")) {
            frame = platform::sdr::make_simulated_spectrum(frame_index_++);
        }
    }
    std::fill(spectrum_pixels_.begin(), spectrum_pixels_.end(), kBackground);
    std::memmove(waterfall_pixels_.data() + kCanvasWidth,
                 waterfall_pixels_.data(),
                 (waterfall_pixels_.size() - kCanvasWidth) * sizeof(uint16_t));

    constexpr int32_t kCenterX = kCanvasWidth / 2 - 1;
    const auto bandwidth_pixels = std::max<int32_t>(
        1,
        (static_cast<int32_t>(view_model().filter_bandwidth_hz()) * kCanvasWidth) / 48000);
    int32_t passband_start = kCenterX;
    int32_t passband_end = kCenterX;
    switch (view_model().radio_mode()) {
    case model::RadioMode::Usb:
    case model::RadioMode::Cw:
        passband_end = std::min(kCanvasWidth - 1, kCenterX + bandwidth_pixels);
        break;
    case model::RadioMode::Lsb:
        passband_start = std::max(0, kCenterX - bandwidth_pixels + 1);
        break;
    case model::RadioMode::Am:
        passband_start = std::max(0, kCenterX - bandwidth_pixels / 2 + 1);
        passband_end = std::min(kCanvasWidth - 1, kCenterX + bandwidth_pixels / 2);
        break;
    }
    for (int32_t x = passband_start; x <= passband_end; ++x) {
        for (int32_t y = 0; y < kSpectrumHeight; ++y) {
            spectrum_pixels_[y * kCanvasWidth + x] = kPassbandColor;
        }
    }

    constexpr uint16_t kGridColor = 0x2104;
    for (int32_t y = 0; y < kSpectrumHeight; y += 6) {
        for (int32_t x = 0; x < kCanvasWidth; ++x) {
            spectrum_pixels_[y * kCanvasWidth + x] = kGridColor;
        }
    }

    int32_t previous_y = kSpectrumHeight - 1;
    for (int32_t x = 0; x < kCanvasWidth; ++x) {
        const auto display_bin = static_cast<size_t>(x) * frame.size() / kCanvasWidth;
        const auto bin = frame.size() - 1 - display_bin;
        const auto level = frame[bin];
        const auto trace_y = kSpectrumHeight - 1
                           - (static_cast<uint16_t>(level) * (kSpectrumHeight - 2)) / 255;
        const int32_t start_y = std::min(previous_y, static_cast<int32_t>(trace_y));
        const int32_t end_y = std::max(previous_y, static_cast<int32_t>(trace_y));
        for (int32_t y = start_y; y <= end_y; ++y) {
            spectrum_pixels_[y * kCanvasWidth + x] = heat_color(level);
        }
        previous_y = trace_y;
        waterfall_pixels_[x] = heat_color(level);
    }

    lv_obj_invalidate(spectrum_canvas_);
    lv_obj_invalidate(waterfall_canvas_);
}

} // namespace screen