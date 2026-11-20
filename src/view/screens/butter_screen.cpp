/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "butter_screen.h"

#include "asset_manager.h"
#include "bindings.h"
namespace screen {

ButterScreen::ButterScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void ButterScreen::build_content(lv_obj_t* content) {
    auto* message = lv_label_create(content);
    lv_label_set_text(message, "This page is intentionally blank");
    auto* font = assets().load_standard_font(16, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(message, font ? font : &lv_font_montserrat_14, 0);
    lv_obj_set_width(message, 288);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(message);
    reactive::bind_theme(message, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
}

} // namespace screen
