#include "asset_manager.h"
#include "application_config.h"
#include "base_viewmodel.h"
#include "butter_screen.h"
#include "receiver_screen.h"
#include "screenshot_service.h"
#include "toast.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

int main() {
    lv_init();
    auto* display = lv_display_create(320, 170);
    assert(display);
    lv_font_glyph_dsc_t glyph{};
    assert(lv_font_get_glyph_dsc(&lv_font_montserrat_14, &glyph, 0xf026, 0));
    assert(lv_font_get_glyph_dsc(&lv_font_montserrat_14, &glyph, 0xf028, 0));

    {
        app::AssetManager assets;
        viewmodel::BaseViewModel view_model;
        view::widgets::RadioDiscoveryNotice notice(lv_layer_top(), view_model, assets);
        notice.build();
        assert(!notice.visible());
        notice.update(model::RadioDiscoveryState::NotFound);
        assert(notice.visible());
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(notice.root(), 0)), "No Radio Found\nConnect Radio") == 0);
        notice.update(model::RadioDiscoveryState::Found);
        assert(!notice.visible());
        notice.update(model::RadioDiscoveryState::Error);
        assert(notice.visible());
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(notice.root(), 0)), "Radio Search Failed\nCheck Network") == 0);
        lv_tick_inc(5001);
        lv_timer_handler();
        assert(!notice.visible());
        notice.update(model::RadioDiscoveryState::Error);
        assert(!notice.visible());
        notice.update(model::RadioDiscoveryState::Found);
        notice.update(model::RadioDiscoveryState::NotFound);
        assert(notice.visible());
        lv_tick_inc(4000);
        lv_timer_handler();
        notice.update(model::RadioDiscoveryState::NotFound);
        lv_tick_inc(1001);
        lv_timer_handler();
        assert(!notice.visible());
        notice.update(model::RadioDiscoveryState::Found);
        assert(view_model.is_dark_mode());
        app::ApplicationConfig config;
        assert(config.dark_mode);
        const auto config_path = std::filesystem::temp_directory_path()
                               / ("square-sdr-theme-test-"
                                  + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                  + ".conf");
        std::string config_error;
        config.dark_mode = false;
        assert(app::save_application_config(config_path.string(), config, config_error));
        app::ApplicationConfig restored;
        assert(app::load_application_config(config_path.string(), restored, config_error));
        assert(!restored.dark_mode);
        std::filesystem::remove(config_path);
        assert(view_model.radio_mode() == model::RadioMode::Usb);
        assert(view_model.filter_bandwidth_hz() == 2800);
        assert(std::strcmp(lv_subject_get_string(view_model.band_mode_subject()), "20m USB") == 0);
        view_model.select_next_radio_mode();
        assert(view_model.radio_mode() == model::RadioMode::Lsb);
        assert(std::strcmp(lv_subject_get_string(view_model.band_mode_subject()), "20m LSB") == 0);
        view_model.cycle_band(1);
        assert(std::strcmp(lv_subject_get_string(view_model.band_mode_subject()), "17m USB") == 0);
        view_model.cycle_band(-1);
        view_model.cycle_filter_bandwidth(1);
        assert(view_model.filter_bandwidth_hz() == 3000);
        view_model.select_next_radio_mode();
        assert(view_model.radio_mode() == model::RadioMode::Lsb);
        assert(view_model.filter_bandwidth_hz() == 2800);
        view_model.select_next_radio_mode();
        assert(view_model.radio_mode() == model::RadioMode::Am);
        assert(view_model.filter_bandwidth_hz() == 4500);
        view_model.select_next_radio_mode();
        assert(view_model.radio_mode() == model::RadioMode::Cw);
        view_model.cycle_filter_bandwidth(-1);
        assert(view_model.filter_bandwidth_hz() == 700);
        view_model.select_next_radio_mode();
        assert(view_model.radio_mode() == model::RadioMode::Usb);
        assert(view_model.filter_bandwidth_hz() == 2800);
        assert(std::strcmp(lv_subject_get_string(view_model.signal_level_subject()), "S --") == 0);
        model::RadioMeasurements measurements;
        measurements.signal_level_dbfs = -100;
        measurements.forward_power_raw = 0;
        measurements.reverse_power_raw = 12;
        measurements.ptt = false;
        measurements.adc_overload = true;
        measurements.forward_watts = 4.94f;
        measurements.swr = 2.0f;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.signal_level_subject()), "~S1") == 0);
        measurements.signal_level_dbfs = -56;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.signal_level_subject()), "~S9") == 0);
        measurements.signal_level_dbfs = -46;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.signal_level_subject()), "~S9+10 dB") == 0);
        measurements.signal_level_dbfs = -36;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.signal_level_subject()), "~S9+20 dB") == 0);
        measurements.signal_level_dbfs = -26;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.signal_level_subject()), "~S9+30 dB") == 0);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_detector_subject()), "FWD 0   REV 12 (ADC)") == 0);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_inputs_subject()), "PTT OFF   ADC overload YES") == 0);
        assert(std::strcmp(lv_subject_get_string(view_model.transmit_meter_subject()), "P ~4.9W  SWR ~2.0:1") == 0);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_alert_subject()), "ADC CLIP") == 0);
        measurements.ptt = true;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_alert_subject()), "PTT ADC CLIP") == 0);
        measurements.adc_overload = false;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_alert_subject()), "PTT") == 0);
        measurements.ptt = false;
        measurements.adc_overload = true;
        view_model.set_radio_measurements({});
        assert(std::strcmp(lv_subject_get_string(view_model.signal_level_subject()), "S --") == 0);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_detector_subject()), "FWD --   REV -- (ADC)") == 0);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_pa_subject()), "PA --C  --A") == 0);
        assert(std::strcmp(lv_subject_get_string(view_model.transmit_meter_subject()), "P --W  SWR --:1") == 0);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_alert_subject()), "") == 0);
        measurements.pa_current_amps = 0.0f;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_pa_subject()), "PA --C  0.00A") == 0);
        measurements.temperature_celsius = 25.0f;
        measurements.forward_power_raw = 4095;
        measurements.reverse_power_raw = 4095;
        measurements.pa_current_amps = 1.0f;
        view_model.set_radio_measurements(measurements);
        assert(std::strcmp(lv_subject_get_string(view_model.radio_pa_subject()), "PA 25.0C  1.00A") == 0);
        auto* previous_screen = lv_screen_active();
        for (const bool dark : {false, true}) {
            view_model.set_dark_mode(dark);
            view_model.show_apple_page();
            screen::ReceiverScreen receiver(view_model, assets);
            lv_screen_load(receiver.root());
            lv_obj_update_layout(receiver.root());
            auto* receiver_content = lv_obj_get_child(receiver.root(), 2);
            auto* signal_label = lv_obj_get_child(receiver_content, 1);
            assert(lv_obj_get_height(signal_label) <= 16);
            auto extreme = measurements;
            extreme.signal_level_dbfs = 0;
            extreme.temperature_celsius = 275.9f;
            extreme.pa_current_amps = 2.07f;
            extreme.forward_watts = 7.3f;
            extreme.swr = 99.0f;
            extreme.ptt = true;
            extreme.adc_overload = true;
            view_model.set_radio_measurements(extreme);
            lv_obj_update_layout(receiver.root());
            for (int index = 0; index < 5; ++index) {
                assert(lv_obj_get_height(lv_obj_get_child(receiver_content, index)) <= (index < 2 ? 16 : 14));
            }
            lv_area_t frequency_area{};
            lv_area_t pa_area{};
            lv_area_t settings_area{};
            lv_area_t power_area{};
            lv_obj_get_coords(lv_obj_get_child(receiver_content, 5), &frequency_area);
            lv_obj_get_coords(lv_obj_get_child(receiver_content, 3), &pa_area);
            lv_obj_get_coords(lv_obj_get_child(receiver_content, 6), &settings_area);
            lv_obj_get_coords(lv_obj_get_child(receiver_content, 4), &power_area);
            assert(frequency_area.x2 < pa_area.x1);
            assert(settings_area.x2 < power_area.x1);
            view_model.set_radio_measurements(measurements);
            lv_obj_update_layout(receiver.root());
            if (std::getenv("SQUARE_SDR_TEST_SCREENSHOTS")) {
                const auto screenshot = platform::screenshot::capture_active_screen();
                assert(screenshot.success);
                std::printf("%s\n", screenshot.path.c_str());
            }
            view::widgets::RadioDiscoveryNotice preview(receiver.root(), view_model, assets);
            preview.update(model::RadioDiscoveryState::NotFound);
            lv_obj_update_layout(receiver.root());
            lv_area_t notice_area{};
            lv_obj_get_coords(preview.root(), &notice_area);
            assert(notice_area.x1 >= 0 && notice_area.x2 < 320);
            assert(notice_area.y1 >= 0 && notice_area.y2 < 170);
            if (std::getenv("SQUARE_SDR_TEST_SCREENSHOTS")) {
                const auto screenshot = platform::screenshot::capture_active_screen();
                assert(screenshot.success);
                std::printf("%s\n", screenshot.path.c_str());
            }
            preview.update(model::RadioDiscoveryState::Found);
            view_model.show_butter_page();
            screen::ButterScreen setup(view_model, assets);
            lv_screen_load(setup.root());
            lv_obj_update_layout(setup.root());
            auto* setup_content = lv_obj_get_child(setup.root(), 2);
            assert(lv_obj_get_child_count(setup_content) == 1);
            assert(std::strcmp(lv_label_get_text(lv_obj_get_child(setup_content, 0)), "This page is intentionally blank") == 0);
            if (std::getenv("SQUARE_SDR_TEST_SCREENSHOTS")) {
                const auto screenshot = platform::screenshot::capture_active_screen();
                assert(screenshot.success);
                std::printf("%s\n", screenshot.path.c_str());
            }
            auto* setup_nav = lv_obj_get_child(setup.root(), 1);
            assert(lv_obj_has_flag(setup_nav, LV_OBJ_FLAG_HIDDEN));
            view_model.show_apple_page();
            assert(view_model.current_page() == model::AppPage::Apple);
            lv_screen_load(previous_screen);
        }
        view::widgets::VolumeOverlay overlay(lv_layer_top(), assets);
        overlay.build();
        auto* root = overlay.root();
        assert(root && lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));

        overlay.show(45, false);
        assert(!lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(root, 1)), "Volume") == 0);
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(root, 2)), "45%") == 0);
        assert(lv_bar_get_value(lv_obj_get_child(root, 3)) == 45);

        overlay.show(80, true);
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(root, 1)), "Muted") == 0);
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(root, 2)), "80%") == 0);
        assert(lv_bar_get_value(lv_obj_get_child(root, 3)) == 80);

        lv_tick_inc(1700);
        lv_timer_handler();
        assert(!lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
        overlay.show(120, false);
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(root, 2)), "100%") == 0);
        assert(lv_bar_get_value(lv_obj_get_child(root, 3)) == 100);
        lv_tick_inc(1801);
        lv_timer_handler();
        assert(lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN));
    }

    lv_display_delete(display);
    lv_deinit();
}