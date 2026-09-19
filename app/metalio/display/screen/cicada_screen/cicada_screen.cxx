/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CicadaScreen — rewritten 竹知了 for ESP32-P4 / NuttX stability.
 * Empty shell on enter; toy UI deferred after Home delete.
 * Touch-drag only (no I2C, no system-audio occupy).
 */

#include "cicada_screen.h"

#include "home_screen/home_screen.h"
#include "i18n.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <unistd.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr int32_t kPanel = 720;
constexpr float kRopeLen = 150.f;
constexpr float kGrav = 900.f;
constexpr float kAir = 0.40f;
constexpr uint32_t kInk = 0x0A1028;
constexpr uint32_t kPaper = 0xF0E6D2;
constexpr uint32_t kAccent = 0xE2603F;
constexpr uint32_t kTube = 0xC9B27A;
constexpr uint32_t kStick = 0xC9A86A;

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_touch = nullptr;
lv_obj_t *s_stick_line = nullptr;
lv_obj_t *s_rope_line = nullptr;
lv_obj_t *s_tube = nullptr;
lv_obj_t *s_rps = nullptr;
lv_obj_t *s_hint = nullptr;
lv_obj_t *s_stats = nullptr;

lv_point_precise_t s_stick_pts[2];
lv_point_precise_t s_rope_pts[2];

lv_timer_t *s_boot = nullptr;
lv_timer_t *s_timer = nullptr;
std::atomic<bool> s_active{false};
bool s_ui_ready = false;

float s_sx = kPanel * 0.5f;
float s_sy = kPanel * 0.40f;
float s_tx = kPanel * 0.5f + 10.f;
float s_ty = kPanel * 0.40f + kRopeLen;
float s_vx = 20.f;
float s_vy = 0.f;
float s_rps_v = 0.f;
float s_prev_th = 0.f;
uint32_t s_wah = 0;
float s_rev = 0.f;
bool s_ptr = false;
int32_t s_last_tick = 0;

void TearDown()
{
    if (s_boot != nullptr) {
        lv_timer_delete(s_boot);
        s_boot = nullptr;
    }
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    s_active.store(false, std::memory_order_relaxed);
    s_ui_ready = false;
    s_ptr = false;
    s_scr = nullptr;
    s_touch = s_stick_line = s_rope_line = s_tube = nullptr;
    s_rps = s_hint = s_stats = nullptr;
}

void OnBack()
{
    TearDown();
    HomeScreen::SwitchToHome();
}

void OnBackClicked(lv_event_t * /*e*/) { OnBack(); }

void UpdateVisual()
{
    if (!s_ui_ready) {
        return;
    }
    s_stick_pts[0].x = s_sx;
    s_stick_pts[0].y = s_sy;
    s_stick_pts[1].x = s_sx;
    s_stick_pts[1].y = s_sy + 70.f;
    if (s_stick_line != nullptr) {
        lv_line_set_points(s_stick_line, s_stick_pts, 2);
    }
    s_rope_pts[0].x = s_sx;
    s_rope_pts[0].y = s_sy;
    s_rope_pts[1].x = s_tx;
    s_rope_pts[1].y = s_ty;
    if (s_rope_line != nullptr) {
        lv_line_set_points(s_rope_line, s_rope_pts, 2);
    }
    if (s_tube != nullptr) {
        lv_obj_set_pos(s_tube, static_cast<int32_t>(s_tx) - 18,
                       static_cast<int32_t>(s_ty) - 8);
    }
}

void UpdateHud()
{
    if (s_rps != nullptr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(s_rps_v));
        lv_label_set_text(s_rps, buf);
    }
    if (s_stats != nullptr) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s %u %s", I18n::T("我"),
                      static_cast<unsigned>(s_wah), I18n::T("哇"));
        lv_label_set_text(s_stats, buf);
    }
}

void Physics(float dt)
{
    if (!s_ptr) {
        s_vy += kGrav * dt;
    }
    s_tx += s_vx * dt;
    s_ty += s_vy * dt;

    float dx = s_tx - s_sx;
    float dy = s_ty - s_sy;
    float d = std::sqrt(dx * dx + dy * dy);
    if (d < 1e-3f) {
        d = 1e-3f;
    }
    if (d > kRopeLen) {
        const float nx = dx / d;
        const float ny = dy / d;
        s_tx = s_sx + nx * kRopeLen;
        s_ty = s_sy + ny * kRopeLen;
        const float vn = s_vx * nx + s_vy * ny;
        if (vn > 0.f) {
            s_vx -= vn * nx;
            s_vy -= vn * ny;
        }
    }
    s_vx *= (1.f - kAir * dt);
    s_vy *= (1.f - kAir * dt);

    const float th = std::atan2(s_ty - s_sy, s_tx - s_sx);
    float dth = th - s_prev_th;
    constexpr float kPi = 3.14159265f;
    constexpr float kTau = 6.2831853f;
    while (dth > kPi) {
        dth -= kTau;
    }
    while (dth < -kPi) {
        dth += kTau;
    }
    s_prev_th = th;
    s_rps_v = std::fabs(dth / dt) / kTau;
    if (s_rps_v > 0.8f) {
        s_rev += std::fabs(dth);
        if (s_rev >= kTau) {
            const int n = static_cast<int>(s_rev / kTau);
            s_rev -= static_cast<float>(n) * kTau;
            s_wah += static_cast<uint32_t>(n);
        }
    }
}

void OnTimer(lv_timer_t * /*t*/)
{
    if (!s_active.load(std::memory_order_relaxed) || !s_ui_ready) {
        return;
    }
    const int32_t now = static_cast<int32_t>(lv_tick_get());
    if (s_last_tick == 0) {
        s_last_tick = now;
    }
    float dt = static_cast<float>((now - s_last_tick) > 0 ? (now - s_last_tick) : 1) /
               1000.f;
    if (dt > 0.1f) {
        dt = 0.1f;
    }
    s_last_tick = now;
    Physics(dt);
    UpdateVisual();
    UpdateHud();
}

void OnPointer(lv_event_t *e)
{
    if (!s_ui_ready) {
        return;
    }
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t pt{};
    if (indev != nullptr) {
        lv_indev_get_point(indev, &pt);
    }
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_ptr = true;
        s_sx = static_cast<float>(pt.x);
        s_sy = static_cast<float>(pt.y);
        if (s_hint != nullptr) {
            lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (code == LV_EVENT_PRESSING) {
        s_ptr = true;
        const float nx = static_cast<float>(pt.x);
        const float ny = static_cast<float>(pt.y);
        s_vx = (nx - s_sx) * 12.f;
        s_vy = (ny - s_sy) * 12.f;
        s_sx = nx;
        s_sy = ny;
    } else {
        s_ptr = false;
    }
}

void BuildUi(lv_obj_t *scr)
{
    s_stick_line = lv_line_create(scr);
    lv_obj_set_style_line_color(s_stick_line, lv_color_hex(kStick), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_stick_line, 6, LV_PART_MAIN);
    lv_obj_remove_flag(s_stick_line, LV_OBJ_FLAG_CLICKABLE);

    s_rope_line = lv_line_create(scr);
    lv_obj_set_style_line_color(s_rope_line, lv_color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_line_width(s_rope_line, 2, LV_PART_MAIN);
    lv_obj_remove_flag(s_rope_line, LV_OBJ_FLAG_CLICKABLE);

    s_tube = lv_obj_create(scr);
    lv_obj_remove_style_all(s_tube);
    lv_obj_set_size(s_tube, 36, 70);
    lv_obj_set_style_radius(s_tube, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_tube, lv_color_hex(kTube), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_tube, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(s_tube, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = lv_obj_create(s_tube);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 36, 14);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(0xCF3B2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(head, 6, LV_PART_MAIN);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);

    s_touch = lv_obj_create(scr);
    lv_obj_remove_style_all(s_touch);
    lv_obj_set_size(s_touch, kPanel, kPanel);
    lv_obj_set_style_bg_opa(s_touch, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_touch, OnPointer, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_touch, OnPointer, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_touch, OnPointer, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_touch, OnPointer, LV_EVENT_PRESS_LOST, nullptr);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 88, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 16, 20);
    lv_obj_set_style_radius(back, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0E142C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, I18n::T("返回"));
    lv_obj_set_style_text_font(bl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, lv_color_hex(kPaper), LV_PART_MAIN);
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, I18n::T("竹知了"));
    lv_obj_set_style_text_font(title, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kPaper), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 120, 24);
    screen_make_input_passive(title);

    s_rps = lv_label_create(scr);
    lv_label_set_text(s_rps, "0.0");
    lv_obj_set_style_text_font(s_rps, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_rps, lv_color_hex(0xFFCF9E), LV_PART_MAIN);
    lv_obj_align(s_rps, LV_ALIGN_TOP_RIGHT, -24, 24);
    screen_make_input_passive(s_rps);

    s_hint = lv_label_create(scr);
    lv_label_set_text(s_hint, I18n::T("按住画圈甩起来"));
    lv_obj_set_style_text_font(s_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(kPaper), LV_PART_MAIN);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -80);
    screen_make_input_passive(s_hint);

    s_stats = lv_label_create(scr);
    lv_label_set_text(s_stats, I18n::T("我 0 哇"));
    lv_obj_set_style_text_font(s_stats, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stats, lv_color_hex(0xFFCF9E), LV_PART_MAIN);
    lv_obj_align(s_stats, LV_ALIGN_BOTTOM_LEFT, 24, -28);
    screen_make_input_passive(s_stats);

    UpdateVisual();
    UpdateHud();
}

void OnBoot(lv_timer_t *t)
{
    s_boot = nullptr;
    lv_timer_delete(t);
    if (!s_active.load(std::memory_order_relaxed) || s_scr == nullptr) {
        return;
    }
    write(1, "CICADA_GO\n", 10);
    BuildUi(s_scr);
    s_ui_ready = true;
    s_last_tick = 0;
    s_timer = lv_timer_create(OnTimer, 140, nullptr);
    write(1, "CICADA_GO_OK\n", 13);
}

void OnUnloaded(lv_event_t *e)
{
    auto *tgt = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (tgt != s_scr) {
        return;
    }
    TearDown();
}

}  // namespace

lv_obj_t *CicadaScreen::CreateStatic()
{
    write(1, "CICADA_CREATE\n", 14);
    TearDown();
    s_wah = 0;
    s_rev = 0.f;
    s_rps_v = 0.f;
    s_vx = 24.f;
    s_vy = 0.f;
    s_sx = kPanel * 0.5f;
    s_sy = kPanel * 0.40f;
    s_tx = s_sx + 10.f;
    s_ty = s_sy + kRopeLen;
    s_prev_th = 0.f;

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_scr = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanel, kPanel);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kInk), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_add_event_cb(scr, OnUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnBack);

    s_active.store(true, std::memory_order_relaxed);
    s_boot = lv_timer_create(OnBoot, 1000, nullptr);
    if (s_boot != nullptr) {
        lv_timer_set_repeat_count(s_boot, 1);
    }
    write(1, "CICADA_BUILT\n", 13);
    return scr;
}

lv_obj_t *CicadaScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void CicadaScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        write(1, "CICADA_LOAD\n", 12);
    } else {
        write(1, "CICADA_UNLOAD\n", 14);
        TearDown();
    }
}
