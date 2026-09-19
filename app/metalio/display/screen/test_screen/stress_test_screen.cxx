#include "stress_test_screen.h"
#include "i18n.h"

#include "application.h"
#include "audio_service.h"
#include "camera_screen/camera_screen.h"
#include "esp_log_shim.h"
#include "stress_demo.h"
#include "pwr_key_handler.h"
#include "screen_util.h"
#include "test_screen.h"
#include "test_ui_common.h"
#include "vibrate_motor_test.h"

#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_50_4);

namespace {

constexpr const char* TAG = "StressTestScreen";

constexpr uint32_t kLvglStressDurationMs = 5 * 60 * 1000;
constexpr uint32_t kMotorDurationMs = 30 * 1000;
constexpr uint32_t kCameraDurationMs = 30 * 1000;
static_assert(kLvglStressDurationMs + kMotorDurationMs + kCameraDurationMs ==
                  6 * 60 * 1000,
              "stress cycle must be 6 minutes");

enum class StressPhase {
    LvglStress,
    MotorVibrate,
    CameraPreview,
};

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_setup_panel = nullptr;
lv_obj_t* s_volume_pct_lbl = nullptr;

StressPhase s_phase = StressPhase::LvglStress;
lv_timer_t* s_cycle_timer = nullptr;
bool s_cycle_running = false;
lv_obj_t* s_cam_overlay = nullptr;
lv_obj_t* s_cam_canvas = nullptr;
bool s_cam_preview_active = false;

void StartStressCycle();
void StopStressCycle();

int ReadStressVolume() {
    int volume = Application::GetInstance().GetAudioService().GetVolume();
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    return volume;
}

void ApplyStressVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    if (s_volume_pct_lbl != nullptr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", volume);
        lv_label_set_text(s_volume_pct_lbl, buf);
    }

    auto& as = Application::GetInstance().GetAudioService();
    if (as.GetVolume() != volume) {
        as.SetVolume(volume);
    }
}

void OnVolumeSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    ApplyStressVolume(static_cast<int>(lv_slider_get_value(slider)));
}

void OnSwipeBackToMenu() {
    if (s_cycle_running) {
        return;
    }
    TestUiNavigateTo(TestScreen::CreateStatic);
}

void OnBackBtnClicked(lv_event_t* /*e*/) {
    OnSwipeBackToMenu();
}

void OnStartStressClicked(lv_event_t* /*e*/) {
    if (s_cycle_running || s_setup_panel == nullptr) {
        return;
    }

    lv_obj_add_flag(s_setup_panel, LV_OBJ_FLAG_HIDDEN);
    StartStressCycle();
}

void BuildSetupPanel(lv_obj_t* scr) {
    s_setup_panel = lv_obj_create(scr);
    screen_strip_obj_chrome(s_setup_panel);
    lv_obj_set_size(s_setup_panel, kTestPanelW, kTestPanelH);
    lv_obj_set_pos(s_setup_panel, 0, 0);
    lv_obj_set_style_bg_color(s_setup_panel, lv_color_hex(kTestColorBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_setup_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_setup_panel, LV_OBJ_FLAG_SCROLLABLE);

    TestUiCreateHeader(s_setup_panel, I18n::T("压力测试"), OnBackBtnClicked);

    const int initial_volume = ReadStressVolume();

    lv_obj_t* card = lv_obj_create(s_setup_panel);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kTestPanelW - 2 * kTestSideMargin, 220);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24);
    lv_obj_set_style_bg_color(card, lv_color_hex(kTestColorCardBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* pct = lv_label_create(card);
    s_volume_pct_lbl = pct;
    lv_obj_set_width(pct, LV_PCT(100));
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(pct, lv_color_hex(0x60A5FA), LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &font_puhui_number_50_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, -20);
    ApplyStressVolume(initial_volume);

    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, I18n::T("系统音量"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_t* slider_row = lv_obj_create(s_setup_panel);
    lv_obj_remove_style_all(slider_row);
    lv_obj_set_size(slider_row, kTestPanelW - 2 * kTestSideMargin, 52);
    lv_obj_align(slider_row, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24 + 220 + 20);
    lv_obj_set_style_bg_opa(slider_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(slider_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(slider_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider = lv_slider_create(slider_row);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, initial_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kTestColorMuted),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3B82F6),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_hor(slider, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 10, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, OnVolumeSliderChanged, LV_EVENT_VALUE_CHANGED,
                        nullptr);
    screen_swipe_back_ignore(slider, true);

    lv_obj_t* range = lv_label_create(s_setup_panel);
    lv_label_set_text(range, "0% ~ 100%");
    lv_obj_set_style_text_color(range, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(range, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(range, LV_ALIGN_TOP_MID, 0, kTestHeaderH + 24 + 220 + 20 + 56);

    lv_obj_t* start = lv_button_create(s_setup_panel);
    lv_obj_set_size(start, 320, 72);
    lv_obj_align(start, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_radius(start, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(start, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_add_event_cb(start, OnStartStressClicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(start, true);

    lv_obj_t* start_lbl = lv_label_create(start);
    lv_label_set_text(start_lbl, I18n::T("开始压力测试"));
    lv_obj_set_style_text_color(start_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(start_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_center(start_lbl);

    lv_obj_t* foot = lv_label_create(s_setup_panel);
    lv_label_set_text(foot, I18n::T("6 分钟循环：LVGL 压测 → 马达 → 摄像头"));
    lv_obj_set_width(foot, kTestPanelW - 2 * kTestSideMargin);
    lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(foot, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -132);
}

void CleanupStressDemoWidgets() {
    if (s_screen == nullptr) {
        return;
    }

    uint32_t i = 0;
    while (i < lv_obj_get_child_count(s_screen)) {
        lv_obj_t* child = lv_obj_get_child(s_screen, i);
        if (child == s_setup_panel) {
            ++i;
            continue;
        }
        lv_obj_delete(child);
    }
}

void StopCameraPreview() {
    if (s_cam_preview_active) {
        CameraScreen::StopExternalPreview();
        s_cam_preview_active = false;
    }

    if (s_cam_canvas != nullptr) {
        lv_obj_delete(s_cam_canvas);
        s_cam_canvas = nullptr;
    }
    if (s_cam_overlay != nullptr) {
        lv_obj_delete(s_cam_overlay);
        s_cam_overlay = nullptr;
    }
}

void StartCameraPreview() {
    StopCameraPreview();

    CameraScreen::PreviewBuffer preview_buf = {};
    if (!CameraScreen::PreparePreviewBuffer(&preview_buf)) {
        ESP_LOGE(TAG, "prepare preview buffer failed");
        return;
    }

    s_cam_overlay = lv_obj_create(lv_layer_top());
    screen_strip_obj_chrome(s_cam_overlay);
    lv_obj_set_size(s_cam_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_cam_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cam_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cam_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_cam_overlay, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_cam_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_cam_canvas = lv_canvas_create(s_cam_overlay);
    lv_canvas_set_buffer(s_cam_canvas, preview_buf.data, preview_buf.width,
                         preview_buf.height, LV_COLOR_FORMAT_RGB888);
    lv_obj_set_size(s_cam_canvas, preview_buf.width, preview_buf.height);
    lv_obj_center(s_cam_canvas);
    screen_make_input_passive(s_cam_canvas);

    if (CameraScreen::StartExternalPreview(s_cam_canvas) != 0) {
        ESP_LOGE(TAG, "start fullscreen preview failed");
        StopCameraPreview();
        return;
    }

    s_cam_preview_active = true;
    ESP_LOGI(TAG, "camera preview started (%dx%d)", preview_buf.width,
             preview_buf.height);
}

void StopCycleTimer() {
    if (s_cycle_timer != nullptr) {
        lv_timer_delete(s_cycle_timer);
        s_cycle_timer = nullptr;
    }
}

void SchedulePhaseTimer(uint32_t duration_ms);

void EnterPhase(StressPhase phase);

void OnCycleTimer(lv_timer_t* /*timer*/) {
    s_cycle_timer = nullptr;
    if (!s_cycle_running) {
        return;
    }

    switch (s_phase) {
    case StressPhase::LvglStress:
        EnterPhase(StressPhase::MotorVibrate);
        break;
    case StressPhase::MotorVibrate:
        EnterPhase(StressPhase::CameraPreview);
        break;
    case StressPhase::CameraPreview:
        EnterPhase(StressPhase::LvglStress);
        break;
    }
}

void SchedulePhaseTimer(uint32_t duration_ms) {
    StopCycleTimer();
    s_cycle_timer = lv_timer_create(OnCycleTimer, duration_ms, nullptr);
    lv_timer_set_repeat_count(s_cycle_timer, 1);
}

void EnterPhase(StressPhase phase) {
    stress_demo_stop();
    CleanupStressDemoWidgets();
    VibrateMotorTest::StopMotor();
    StopCameraPreview();

    s_phase = phase;

    uint32_t duration_ms = 0;
    switch (phase) {
    case StressPhase::LvglStress:
        ESP_LOGI(TAG, "phase: LVGL stress (%lus)",
                 static_cast<unsigned long>(kLvglStressDurationMs / 1000));
        stress_demo_start();
        duration_ms = kLvglStressDurationMs;
        break;
    case StressPhase::MotorVibrate:
        ESP_LOGI(TAG, "phase: motor vibrate (%lus)",
                 static_cast<unsigned long>(kMotorDurationMs / 1000));
        VibrateMotorTest::StartMotor();
        duration_ms = kMotorDurationMs;
        break;
    case StressPhase::CameraPreview:
        ESP_LOGI(TAG, "phase: camera preview (%lus)",
                 static_cast<unsigned long>(kCameraDurationMs / 1000));
        StartCameraPreview();
        duration_ms = kCameraDurationMs;
        break;
    }

    if (s_cycle_running) {
        SchedulePhaseTimer(duration_ms);
    }
}

void StartStressCycle() {
    StopStressCycle();

    auto& app = Application::GetInstance();
    app.SetActivationSuspended(true);
    app.StopSystemAudioForStressTest();

    VibrateMotorTest::OnLoad();

    s_cycle_running = true;
    EnterPhase(StressPhase::LvglStress);
}

void StopStressCycle() {
    s_cycle_running = false;
    StopCycleTimer();
    stress_demo_stop();
    CleanupStressDemoWidgets();
    VibrateMotorTest::StopMotor();
    VibrateMotorTest::OnUnload();
    StopCameraPreview();
    auto& app = Application::GetInstance();
    app.SetActivationSuspended(false);
    app.RestoreSystemAudioAfterStressTest();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    StopStressCycle();
    s_screen = nullptr;
    s_setup_panel = nullptr;
    s_volume_pct_lbl = nullptr;
}

void stress_test_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("stress_test", event);
}

}  // namespace

lv_obj_t* StressTestScreen::Create() {
    ESP_LOGI(TAG, "create stress test screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kTestColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildSetupPanel(scr);

    screen_attach_lifecycle(scr, stress_test_lifecycle_cb);
    screen_attach_swipe_back(scr, OnSwipeBackToMenu);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}
