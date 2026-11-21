/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "navbar.h"

#include "asset_manager.h"
#include "bindings.h"
#include "linux_input.h"
#include "theme.h"
#include "ui_const.h"

namespace view::widgets {
namespace {

struct IconSpec {
    const char* text;
    lv_event_cb_t click_cb;
};

constexpr std::array<int32_t, 5> kNavButtonXOffsets = {20, 12, 2, -10, -20};

struct ShortcutSpec {
    const char* key;
    const char* action;
};

constexpr std::array<ShortcutSpec, 2> kButterShortcuts = {{
    {"F/X", "Select"},
    {"Z/C", "Change"},
}};

} // namespace

NavBar::NavBar(lv_obj_t* parent, viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseWidgets(parent), view_model_(view_model), assets_(assets) {}

NavBar::~NavBar() {
    for (size_t i = 0; i < icon_buttons_.size(); ++i) {
        if (icon_buttons_[i]) {
            platform::unregister_nav_button(i, icon_buttons_[i]->root());
        }
    }

    if (page_observer_) {
        lv_observer_remove(page_observer_);
        page_observer_ = nullptr;
    }
    if (theme_observer_) {
        lv_observer_remove(theme_observer_);
        theme_observer_ = nullptr;
    }
}

void NavBar::build() {
    if (core_obj_) {
        return;
    }

    core_obj_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(core_obj_);
    lv_obj_set_size(core_obj_, LV_PCT(100), kNavBarHeight);
    lv_obj_align(core_obj_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(core_obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(core_obj_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(core_obj_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(core_obj_, 8, 0);
    lv_obj_set_style_pad_right(core_obj_, 8, 0);
    reactive::bind_theme(core_obj_, view_model_.dark_mode_subject(), reactive::ThemeRole::Bar);

    create_icon_buttons();
    create_shortcut_hints();
    page_observer_ = reactive::observe_obj(core_obj_, view_model_.current_page_subject(), update_icons_cb, this);
    theme_observer_ = reactive::observe_obj(core_obj_, view_model_.dark_mode_subject(), update_icons_cb, this);
    update_icon_buttons();
}

void NavBar::create_shortcut_hints() {
    shortcut_bar_ = lv_obj_create(core_obj_);
    lv_obj_remove_style_all(shortcut_bar_);
    lv_obj_set_size(shortcut_bar_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(shortcut_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(shortcut_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(shortcut_bar_, 10, 0);
    lv_obj_clear_flag(shortcut_bar_, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t* key_font = assets_.load_standard_font(14);
    if (!key_font) {
        key_font = &lv_font_montserrat_14;
    }
    const lv_font_t* text_font = assets_.load_standard_font(14, app::StandardFontWeight::Regular);
    if (!text_font) {
        text_font = &lv_font_montserrat_14;
    }

    for (size_t index = 0; index < kButterShortcuts.size(); ++index) {
        const auto& shortcut = kButterShortcuts[index];
        auto* item = lv_obj_create(shortcut_bar_);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, LV_SIZE_CONTENT, LV_PCT(100));
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, 3, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        auto* key = lv_label_create(item);
        lv_label_set_text(key, shortcut.key);
        lv_obj_set_style_text_font(key, key_font, 0);
        shortcut_key_labels_[index] = key;

        auto* label = lv_label_create(item);
        lv_label_set_text(label, shortcut.action);
        lv_obj_set_style_text_font(label, text_font, 0);
        reactive::bind_theme(label, view_model_.dark_mode_subject(), reactive::ThemeRole::Text);
    }
}

void NavBar::create_icon_buttons() {
    const auto light_color = view::palette(false).text;
    const auto dark_color = view::palette(true).text;
    icon_font_ = assets_.load_standard_font(10, app::StandardFontWeight::Regular);

    for (size_t i = 0; i < icon_buttons_.size(); ++i) {
        icon_buttons_[i] = std::make_unique<IconButton>(core_obj_,
                                                        view_model_,
                                52,
                                                        24,
                                                        "",
                                                        icon_font_ ? icon_font_ : &lv_font_montserrat_14,
                                                        light_color,
                                                        dark_color,
                                                        nullptr,
                                                        &view_model_,
                                                        true);
        icon_buttons_[i]->build();
        lv_obj_set_style_translate_x(icon_buttons_[i]->root(), kNavButtonXOffsets[i], 0);
        platform::register_nav_button(i, icon_buttons_[i]->root());
    }
}

void NavBar::update_icon_buttons() {
    const bool is_butter_page = view_model_.current_page() == model::AppPage::Butter;
    const bool is_dark_mode = view_model_.is_dark_mode();
    if (is_butter_page) lv_obj_add_flag(core_obj_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(core_obj_, LV_OBJ_FLAG_HIDDEN);
    const auto colors = view::palette(is_dark_mode);
    /* nav bar icons showed on apple screen */
    const std::array<IconSpec, 5> apple_icons = {{
        {"Band", cycle_band_cb},
        {"Mode", select_next_mode_cb},
        {"BW", cycle_bandwidth_cb},
        {"Step", cycle_step_cb},
        {"Setup", toggle_page_cb},
    }};
    /* Hidden action targets for the shortcut keys shown on the butter screen. */
    const std::array<IconSpec, 5> butter_icons = {{
        {"", nullptr},
        {"", select_previous_mode_cb},
        {"", nullptr},
        {"", select_next_mode_cb},
        {"Back", toggle_page_cb},
    }};

    platform::set_nav_shortcut_mode(is_butter_page);
    for (auto* key_label : shortcut_key_labels_) {
        if (key_label) {
            lv_obj_set_style_text_color(key_label, colors.primary, 0);
        }
    }
    if (shortcut_bar_) {
        lv_obj_add_flag(shortcut_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    const auto& icons = is_butter_page ? butter_icons : apple_icons;
    lv_obj_set_flex_align(core_obj_,
                          is_butter_page ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    for (size_t i = 0; i < icon_buttons_.size(); ++i) {
        auto& button = icon_buttons_[i];
        if (!button) {
            continue;
        }

        button->set_text(icons[i].text);
        if (button->root()) {
            lv_obj_set_style_translate_x(button->root(), is_butter_page ? 0 : kNavButtonXOffsets[i], 0);
            if (is_butter_page && i != 4) {
                lv_obj_add_flag(button->root(), LV_OBJ_FLAG_HIDDEN);
            }
            else {
                lv_obj_remove_flag(button->root(), LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_event_cb(button->root(), toggle_theme_cb);
            lv_obj_remove_event_cb(button->root(), toggle_page_cb);
            lv_obj_remove_event_cb(button->root(), cycle_band_cb);
            lv_obj_remove_event_cb(button->root(), select_next_mode_cb);
            lv_obj_remove_event_cb(button->root(), cycle_bandwidth_cb);
            lv_obj_remove_event_cb(button->root(), cycle_step_cb);
            if (icons[i].click_cb) {
                lv_obj_add_event_cb(button->root(), icons[i].click_cb, LV_EVENT_CLICKED, &view_model_);
                button->set_enabled(true);
            }
            else {
                button->set_enabled(false);
            }
        }
    }
}

void NavBar::toggle_theme_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->toggle_dark_mode();
    }
}

void NavBar::toggle_page_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->toggle_page();
    }
}

void NavBar::select_next_mode_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->select_next_radio_mode();
    }
}

void NavBar::select_previous_mode_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->select_previous_radio_mode();
    }
}

void NavBar::cycle_band_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->cycle_band(1);
    }
}

void NavBar::cycle_bandwidth_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->cycle_filter_bandwidth(1);
    }
}

void NavBar::cycle_step_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (view_model) {
        view_model->cycle_tuning_step(1);
    }
}

void NavBar::update_icons_cb(lv_observer_t* observer, lv_subject_t* subject) {
    LV_UNUSED(subject);

    auto* nav_bar = static_cast<NavBar*>(lv_observer_get_user_data(observer));
    if (nav_bar) {
        nav_bar->update_icon_buttons();
    }
}

} // namespace view::widgets
