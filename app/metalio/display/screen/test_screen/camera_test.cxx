#include "camera_test.h"
#include "i18n.h"

#include <cstdio>

#include "camera_screen/camera_screen.h"
#include "esp_log_shim.h"
#include "screen_util.h"
#include "test_ui_common.h"
#include <metalio/metalio.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "CameraTest";

constexpr uint32_t kColorBtnIdle = 0x2563EB;
constexpr int      kPreviewAreaW  = 720;
constexpr int      kPreviewAreaH  = 600;
constexpr int      kPreviewStripH = 120;

lv_obj_t* s_status_icon    = nullptr;
lv_obj_t* s_value_lbl      = nullptr;
lv_obj_t* s_preview_btn    = nullptr;
lv_obj_t* s_preview_mask   = nullptr;
lv_obj_t* s_preview_canvas = nullptr;

bool s_preview_started = false;

void SetErrorText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorError),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, false);
}

void SetPassText(const char* msg) {
    if (s_value_lbl == nullptr) {
        return;
    }
    lv_label_set_text(s_value_lbl, msg);
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    TestUiUpdateStatus(s_status_icon, true);
}

void ClosePreviewOverlay() {
    if (s_preview_started) {
        CameraScreen::StopExternalPreview();
        s_preview_started = false;
    }
    if (s_preview_mask != nullptr) {
        lv_obj_delete(s_preview_mask);
        s_preview_mask = nullptr;
        s_preview_canvas = nullptr;
    }
}

void OnClosePreviewClicked(lv_event_t* /*e*/) {
    ClosePreviewOverlay();
}

void OpenPreviewOverlay() {
    if (s_preview_mask != nullptr) {
        return;
    }

    lv_obj_t* parent = TestUiGetScreen();
    if (parent == nullptr) {
        return;
    }

    CameraScreen::PreviewBuffer preview_buf = {};
    if (!CameraScreen::PreparePreviewBuffer(&preview_buf)) {
        ESP_LOGE(TAG, "prepare preview buffer failed");
        SetErrorText(I18n::T("预览缓冲失败"));
        return;
    }

    lv_obj_t* mask = lv_obj_create(parent);
    s_preview_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kTestPanelW, kTestPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(mask, true);

    lv_obj_t* canvas_host = lv_obj_create(mask);
    screen_strip_obj_chrome(canvas_host);
    lv_obj_set_size(canvas_host, kPreviewAreaW, kPreviewAreaH);
    lv_obj_set_pos(canvas_host, 0, 0);
    lv_obj_set_style_bg_color(canvas_host, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(canvas_host, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(canvas_host, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(canvas_host);

    s_preview_canvas = lv_canvas_create(canvas_host);
    lv_canvas_set_buffer(s_preview_canvas, preview_buf.data, preview_buf.width,
                         preview_buf.height, LV_COLOR_FORMAT_RGB888);
    lv_obj_set_size(s_preview_canvas, preview_buf.width, preview_buf.height);
    lv_obj_set_pos(s_preview_canvas, 0, 0);
    screen_make_input_passive(s_preview_canvas);

    lv_obj_t* strip = lv_obj_create(mask);
    screen_strip_obj_chrome(strip);
    lv_obj_set_size(strip, kTestPanelW, kPreviewStripH);
    lv_obj_set_pos(strip, 0, kPreviewAreaH);
    lv_obj_set_style_bg_color(strip, lv_color_hex(kTestColorCardBg),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(strip, true);

    lv_obj_t* title = lv_label_create(strip);
    lv_label_set_text(title, I18n::T("摄像头预览"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 28, 0);
    screen_make_input_passive(title);

    lv_obj_t* close_btn = lv_button_create(strip);
    lv_obj_remove_style_all(close_btn);
    lv_obj_set_size(close_btn, 140, 56);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_obj_set_style_radius(close_btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, LV_PART_MAIN);
    screen_swipe_back_ignore(close_btn, true);

    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, I18n::T("关闭"));
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(close_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(close_lbl);
    lv_obj_remove_flag(close_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(close_btn, OnClosePreviewClicked, LV_EVENT_CLICKED,
                        nullptr);

    if (CameraScreen::StartExternalPreview(s_preview_canvas) != 0) {
        ESP_LOGE(TAG, "start preview failed");
        ClosePreviewOverlay();
        SetErrorText(I18n::T("预览启动失败"));
        return;
    }

    s_preview_started = true;
    ESP_LOGI(TAG, "preview overlay started (%dx%d)", preview_buf.width,
             preview_buf.height);
}

void OnPreviewBtnClicked(lv_event_t* /*e*/) {
    OpenPreviewOverlay();
}

}  // namespace

namespace CameraTest {

void BuildRow(lv_obj_t* list) {
    lv_obj_t* ctrl = nullptr;
    TestUiCreateRowShell(list, I18n::T("摄像头"), &s_status_icon, &ctrl);

    s_value_lbl = lv_label_create(ctrl);
    lv_label_set_text(s_value_lbl, I18n::T("检测中..."));
    lv_obj_set_style_text_color(s_value_lbl, lv_color_hex(kTestColorTextDim),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_value_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_label_set_long_mode(s_value_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_value_lbl, 1);
    lv_obj_set_style_text_align(s_value_lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    s_preview_btn = lv_button_create(ctrl);
    lv_obj_remove_style_all(s_preview_btn);
    lv_obj_set_size(s_preview_btn, 120, 52);
    lv_obj_set_style_radius(s_preview_btn, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_preview_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_preview_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(s_preview_btn, OnPreviewBtnClicked, LV_EVENT_CLICKED,
                        nullptr);
    screen_swipe_back_ignore(s_preview_btn, true);

    lv_obj_t* preview_lbl = lv_label_create(s_preview_btn);
    lv_label_set_text(preview_lbl, I18n::T("预览"));
    lv_obj_set_style_text_color(preview_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(preview_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(preview_lbl);
    lv_obj_remove_flag(preview_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void OnLoad() {
    s_preview_started = false;
    if (metalio_camera_power(false) < 0) {
        SetErrorText(I18n::T("上电失败"));
        return;
    }
    SetPassText(I18n::T("OV2710 已上电"));
}

void OnUnload() {
    ClosePreviewOverlay();
    metalio_camera_power(true);
    s_value_lbl   = nullptr;
    s_status_icon = nullptr;
    s_preview_btn = nullptr;
}

void Poll() {
    // SCCB ID 探测需要用户态 I2C 读接口（openvela 暂未提供），
    // 这里仅确认上电状态；预览走 CameraScreen 视频管线。
}

}  // namespace CameraTest
