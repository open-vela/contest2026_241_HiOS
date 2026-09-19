/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * TestScreen — ported from MetalioClaw4
 * main/display/screen/test_screen/test_screen.cc.
 *
 * The original test menu launches several hardware-test sub-screens
 * (AutoTestScreen, StressTestScreen, ScreenColorTest, TouchPanelTest)
 * implemented in sibling .cc files. Those sub-screens depend heavily on
 * ESP-IDF drivers (I2C, audio codec, camera, GPS) and are not yet
 * ported; the menu is wired up here and the sub-screen launches are
 * stubbed with TODO markers so the menu itself works.
 */

#include "test_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "home_screen/home_screen.h"
#include "screen_color_test.h"
#include "touch_panel_test.h"
#include "auto_test_screen.h"
#include "stress_test_screen.h"

namespace {

constexpr const char *TAG = "TestScreen";

// Panel + palette used by every test sub-screen (ported from
// test_ui_common.cc).
constexpr int kTestPanelW = 720;
constexpr int kTestPanelH = 720;
constexpr uint32_t kTestColorBg = 0x0E1116;
constexpr uint32_t kTestColorText = 0xFFFFFF;
constexpr uint32_t kTestColorSubtle = 0x9AA3B2;
constexpr uint32_t kTestColorCard = 0x1B2030;
constexpr uint32_t kTestColorAccent = 0x3B82F6;

lv_obj_t *s_screen = nullptr;
screen_lifecycle_cb_t s_lifecycle_cb = nullptr;

void OnSwipeBackHome()
{
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    HomeScreen::SwitchToHome();
}

void OnBackBtnClicked(lv_event_t * /*e*/) { OnSwipeBackHome(); }

void NavigateToSubScreen(lv_obj_t *(*create_screen)())
{
    if (create_screen == nullptr) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    lv_obj_t *old_scr = lv_screen_active();
    lv_obj_t *new_scr = create_screen();
    lv_screen_load(new_scr);
    if (old_scr != nullptr && old_scr != new_scr) {
        lv_obj_delete_async(old_scr);
    }
}

void OnAutoTestClicked(lv_event_t * /*e*/)
{
    NavigateToSubScreen(AutoTestScreen::Create);
}

void OnStressTestClicked(lv_event_t * /*e*/)
{
    NavigateToSubScreen(StressTestScreen::Create);
}

void OnScreenColorTestClicked(lv_event_t * /*e*/)
{
    NavigateToSubScreen(ScreenColorTest::Create);
}

void OnTouchPanelTestClicked(lv_event_t * /*e*/)
{
    NavigateToSubScreen(TouchPanelTest::Create);
}

void OnScreenUnloaded(lv_event_t * /*e*/) { s_screen = nullptr; }

// Local re-implementation of test_ui_common helpers (header + scroll body
// + menu rows), keeping the same look as the original test menu.
void CreateHeader(lv_obj_t *parent, const char *title,
                  lv_event_cb_t on_back)
{
    lv_obj_t *header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kTestPanelW, 90);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 72, 72);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20,
                            LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);

    lv_obj_t *icon = lv_image_create(back);
    lv_image_set_src(icon, "A:ic_app_back.spng");
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(kTestColorText),
                                LV_PART_MAIN);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 16 + 72 + 16, 0);
}

lv_obj_t *CreateScrollBody(lv_obj_t *parent)
{
    lv_obj_t *body = lv_obj_create(parent);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kTestPanelW - 32, kTestPanelH - 90 - 16);
    lv_obj_set_pos(body, 16, 90);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 12, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    return body;
}

void CreateMenuRow(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 80);
    lv_obj_set_style_bg_color(row, lv_color_hex(kTestColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 24, LV_PART_MAIN);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(row, true);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kTestColorText),
                                LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
}

}  // namespace

lv_obj_t *TestScreen::CreateStatic()
{
    ESP_LOGI(TAG, "create test screen menu");

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kTestPanelW, kTestPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kTestColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    CreateHeader(scr, I18n::T("硬件测试"), OnBackBtnClicked);

    lv_obj_t *body = CreateScrollBody(scr);
    CreateMenuRow(body, I18n::T("自动测试"), OnAutoTestClicked);
    CreateMenuRow(body, I18n::T("压力测试"), OnStressTestClicked);
    CreateMenuRow(body, I18n::T("屏幕测试"), OnScreenColorTestClicked);
    CreateMenuRow(body, I18n::T("触摸测试"), OnTouchPanelTestClicked);

    if (s_lifecycle_cb != nullptr) {
        screen_attach_lifecycle(scr, s_lifecycle_cb);
    }

    screen_attach_swipe_back(scr, OnSwipeBackHome);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);

    return scr;
}

lv_obj_t *TestScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void TestScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: test_screen");
    } else {
        ESP_LOGI(TAG, "unload: test_screen");
    }
}

void TestScreen::LaunchFromHome(screen_lifecycle_cb_t lifecycle_cb)
{
    s_lifecycle_cb = lifecycle_cb;
    lv_obj_t *old_scr = lv_screen_active();
    lv_obj_t *app = TestScreen::CreateStatic();
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}
