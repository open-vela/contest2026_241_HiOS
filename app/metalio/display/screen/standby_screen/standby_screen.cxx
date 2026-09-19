/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * StandbyScreen — ported from MetalioClaw4
 * main/display/screen/standby_screen/standby_screen.cc.
 *
 * Flip-clock from Claw4. Charge particles on this NuttX port MUST NOT use:
 *   - LV_RADIUS_CIRCLE / lv_draw_rect circles (blow 24KB draw-layer)
 *   - large RGB565 canvas (~144KB) (hangs before SB_CHG_CANVAS on PSRAM)
 * Use a few small square objs (radius 0) + tip label only.
 */

#include "standby_screen.h"
#include "i18n.h"
#include "esp_log_shim.h"
#include "board_shim.h"
#include "home_screen/home_screen.h"
#include "idle_power_policy/idle_power_policy.h"
#include "pwr_key_handler/pwr_key_handler.h"
#include "screen_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_puhui_number_120_4);

namespace {

constexpr const char *TAG = "StandbyScreen";
constexpr int kPanelSize = 720;

constexpr int kChargeFxW = 360;
constexpr int kChargeFxH = 140;
constexpr int kParticleCount = 12;
constexpr uint32_t kChargeBlueSoft = 0x59B2FF;
constexpr uint32_t kChargeBlueBright = 0x9AD0FF;
constexpr uint32_t kChargeEffectMs = 10000;
constexpr uint32_t kParticleTickMs = 80;

// Flip-clock: HH MM SS — three groups, two digits each.
constexpr int kDigitCount = 6;
constexpr int kDigitW = 88;
constexpr int kDigitH = 148;
constexpr int kDigitHalf = kDigitH / 2;
constexpr int kPairGap = 6;
constexpr int kGroupGap = 32;
constexpr uint32_t kCardBg = 0x1C1C1E;
constexpr uint32_t kCardBgTop = 0x2A2A2E;
constexpr uint32_t kHingeColor = 0x0A0A0A;
constexpr uint32_t kDigitColor = 0xF5F5F7;
constexpr int32_t kFlipProgressMax = kDigitHalf * 2;
constexpr uint32_t kFlipDurationMs = 480;

constexpr const char *kWeekdayMsgIds[7] = {"日", "一", "二", "三",
                                            "四", "五", "六"};

/* Square dots only — never LV_RADIUS_CIRCLE on this port. */
struct Particle {
    lv_obj_t *obj = nullptr;
    float x = 0;
    float y = 0;
    float vx = 0;
    float vy = 0;
    float size = 4;
    float life = 0;
    float life_decay = 0;
    uint32_t color = kChargeBlueSoft;
    bool alive = false;
};

struct FlipDigit {
    lv_obj_t *card = nullptr;
    lv_obj_t *top_clip = nullptr;
    lv_obj_t *top_lbl = nullptr;
    lv_obj_t *bot_clip = nullptr;
    lv_obj_t *bot_lbl = nullptr;
    lv_obj_t *flap = nullptr;
    lv_obj_t *flap_lbl = nullptr;
    lv_obj_t *hinge = nullptr;
    lv_obj_t *shade = nullptr;
    int32_t label_ofs_y = 0;
    char current = '0';
    char target = 0;
    char pending = 0;
    char old_ch = '0';
    bool flap_lower = false;
    bool animating = false;
};

struct UiState {
    lv_obj_t *screen = nullptr;
    lv_obj_t *clock_row = nullptr;
    FlipDigit digits[kDigitCount]{};
    lv_obj_t *date_lbl = nullptr;
    lv_timer_t *update_timer = nullptr;

    lv_obj_t *charge_root = nullptr;
    lv_obj_t *charge_tip = nullptr;
    Particle particles[kParticleCount]{};
    lv_timer_t *charge_tick_timer = nullptr;
    lv_timer_t *charge_stop_timer = nullptr;
    lv_timer_t *charge_defer_timer = nullptr;
    uint32_t charge_rng = 1;
    bool charge_playing = false;
    bool last_charging = false;
    bool charge_primed = false;
    bool clock_primed = false;
};

UiState s_ui;

void FlipDigitSetChar(lv_obj_t *lbl, char ch)
{
    if (lbl == nullptr) return;
    char buf[2] = {ch, '\0'};
    lv_label_set_text(lbl, buf);
}

void FlipDigitRaiseDecor(FlipDigit *d)
{
    if (d == nullptr) return;
    if (d->shade != nullptr) lv_obj_move_foreground(d->shade);
    if (d->hinge != nullptr) lv_obj_move_foreground(d->hinge);
}

void FlipDigitApplyProgress(FlipDigit *d, int32_t progress)
{
    if (d == nullptr || d->flap == nullptr) return;
    if (progress < 0) progress = 0;
    if (progress > kFlipProgressMax) progress = kFlipProgressMax;

    const bool lower = progress > kDigitHalf;
    if (lower != d->flap_lower) {
        d->flap_lower = lower;
        if (lower) {
            FlipDigitSetChar(d->flap_lbl, d->target);
            lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBg),
                                      LV_PART_MAIN);
        } else {
            FlipDigitSetChar(d->flap_lbl, d->old_ch);
            lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBgTop),
                                      LV_PART_MAIN);
        }
    }

    int32_t h;
    int32_t y;
    int32_t lbl_y;
    if (!lower) {
        h = kDigitHalf - progress;
        if (h < 1) h = 1;
        y = kDigitHalf - h;
        lbl_y = d->label_ofs_y - y;
    } else {
        h = progress - kDigitHalf;
        if (h < 1) h = 1;
        y = kDigitHalf;
        lbl_y = d->label_ofs_y - kDigitHalf;
    }

    lv_obj_set_pos(d->flap, 0, y);
    lv_obj_set_height(d->flap, h);
    lv_obj_set_y(d->flap_lbl, lbl_y);

    const int32_t dist = lower ? (kFlipProgressMax - progress) : progress;
    const lv_opa_t shade = static_cast<lv_opa_t>(dist * 160 / kDigitHalf);
    if (d->shade != nullptr) {
        lv_obj_set_pos(d->shade, 0, y);
        lv_obj_set_height(d->shade, h);
        lv_obj_set_style_bg_opa(d->shade, shade, LV_PART_MAIN);
        lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
    }
}

void FlipDigitFinish(FlipDigit *d, char ch);
void FlipDigitStartFlip(FlipDigit *d, char next);

void FlipDigitFinish(FlipDigit *d, char ch)
{
    if (d == nullptr) return;
    FlipDigitSetChar(d->top_lbl, ch);
    FlipDigitSetChar(d->bot_lbl, ch);
    d->current = ch;
    d->target = 0;
    d->flap_lower = false;
    if (d->flap != nullptr) lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
    if (d->shade != nullptr) lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
    d->animating = false;

    const char queued = d->pending;
    d->pending = 0;
    if (queued != 0 && queued != d->current) {
        FlipDigitStartFlip(d, queued);
    }
}

void FlipDigitStartFlip(FlipDigit *d, char next)
{
    if (d == nullptr || d->card == nullptr || d->flap == nullptr) return;
    if (next == d->current && !d->animating) return;
    if (d->animating) {
        d->pending = next;
        return;
    }

    d->old_ch = d->current;
    d->target = next;
    d->animating = true;
    d->flap_lower = false;

    FlipDigitSetChar(d->top_lbl, next);
    FlipDigitSetChar(d->bot_lbl, d->old_ch);
    FlipDigitSetChar(d->flap_lbl, d->old_ch);
    lv_obj_set_style_bg_color(d->flap, lv_color_hex(kCardBgTop), LV_PART_MAIN);

    lv_obj_remove_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
    FlipDigitApplyProgress(d, 0);
    FlipDigitRaiseDecor(d);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_values(&a, 0, kFlipProgressMax);
    lv_anim_set_duration(&a, kFlipDurationMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, d);
    lv_anim_set_exec_cb(&a, [](void *var, int32_t v) {
        FlipDigitApplyProgress(static_cast<FlipDigit *>(var), v);
    });
    lv_anim_set_completed_cb(&a, [](lv_anim_t *anim) {
        FlipDigit *digit =
            static_cast<FlipDigit *>(lv_anim_get_user_data(anim));
        if (digit == nullptr) return;
        FlipDigitFinish(digit, digit->target);
    });
    lv_anim_start(&a);
}

void FlipDigitSet(FlipDigit *d, char ch, bool animate)
{
    if (d == nullptr || ch < '0' || ch > '9') return;
    if (!animate || d->card == nullptr) {
        lv_anim_delete(d, nullptr);
        d->pending = 0;
        d->target = 0;
        d->animating = false;
        d->flap_lower = false;
        FlipDigitSetChar(d->top_lbl, ch);
        FlipDigitSetChar(d->bot_lbl, ch);
        d->current = ch;
        if (d->flap != nullptr) lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
        if (d->shade != nullptr) lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (ch == d->current && !d->animating) return;
    FlipDigitStartFlip(d, ch);
}

lv_obj_t *CreateHalfClip(lv_obj_t *parent, int y, uint32_t bg)
{
    lv_obj_t *clip = lv_obj_create(parent);
    lv_obj_remove_style_all(clip);
    lv_obj_set_size(clip, kDigitW, kDigitHalf);
    lv_obj_set_pos(clip, 0, y);
    lv_obj_set_style_bg_color(clip, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(clip, true, LV_PART_MAIN);
    lv_obj_set_style_radius(clip, 0, LV_PART_MAIN);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return clip;
}

lv_obj_t *CreateDigitLabel(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "0");
    lv_obj_set_style_text_font(lbl, &font_puhui_number_120_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kDigitColor), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(lbl, kDigitW);
    lv_obj_set_pos(lbl, 0, y);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void CreateFlipDigit(lv_obj_t *parent, FlipDigit *d)
{
    const int32_t line_h = font_puhui_number_120_4.line_height;
    d->label_ofs_y = (kDigitH - line_h) / 2;

    d->card = lv_obj_create(parent);
    lv_obj_remove_style_all(d->card);
    lv_obj_set_size(d->card, kDigitW, kDigitH);
    lv_obj_set_style_radius(d->card, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d->card, lv_color_hex(kCardBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(d->card, true, LV_PART_MAIN);
    lv_obj_remove_flag(d->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d->card, LV_OBJ_FLAG_CLICKABLE);

    d->top_clip = CreateHalfClip(d->card, 0, kCardBgTop);
    d->top_lbl = CreateDigitLabel(d->top_clip, d->label_ofs_y);

    d->bot_clip = CreateHalfClip(d->card, kDigitHalf, kCardBg);
    d->bot_lbl = CreateDigitLabel(d->bot_clip, d->label_ofs_y - kDigitHalf);

    d->flap = CreateHalfClip(d->card, 0, kCardBgTop);
    d->flap_lbl = CreateDigitLabel(d->flap, d->label_ofs_y);
    lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);

    d->shade = lv_obj_create(d->card);
    lv_obj_remove_style_all(d->shade);
    lv_obj_set_size(d->shade, kDigitW, kDigitHalf);
    lv_obj_set_style_bg_color(d->shade, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->shade, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);

    d->hinge = lv_obj_create(d->card);
    lv_obj_remove_style_all(d->hinge);
    lv_obj_set_size(d->hinge, kDigitW, 3);
    lv_obj_set_pos(d->hinge, 0, kDigitHalf - 1);
    lv_obj_set_style_bg_color(d->hinge, lv_color_hex(kHingeColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->hinge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(d->hinge, LV_OBJ_FLAG_CLICKABLE);

    d->current = '0';
    d->target = 0;
    d->pending = 0;
    d->old_ch = '0';
    d->flap_lower = false;
    d->animating = false;
}

lv_obj_t *CreateFlipClockRow(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kDigitCount; ++i) {
        if (i > 0) {
            const bool group_break = (i % 2 == 0);
            lv_obj_t *gap = lv_obj_create(row);
            lv_obj_remove_style_all(gap);
            lv_obj_set_size(gap, group_break ? kGroupGap : kPairGap, 1);
            lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_remove_flag(gap, LV_OBJ_FLAG_CLICKABLE);
        }
        CreateFlipDigit(row, &s_ui.digits[i]);
    }
    return row;
}

void StopChargeEffect();
void StartChargeEffect(int battery_level);

uint32_t ChargeRand()
{
    uint32_t x = s_ui.charge_rng ? s_ui.charge_rng : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_ui.charge_rng = x;
    return x;
}

float ChargeRand01()
{
    return static_cast<float>(ChargeRand() & 0xFFFFu) / 65535.0f;
}

void ApplyParticleVisual(Particle *p)
{
    if (p == nullptr || p->obj == nullptr)
        return;
    int sz = static_cast<int>(p->size * (0.50f + 0.50f * p->life));
    if (sz < 2)
        sz = 2;
    if (sz > 8)
        sz = 8;
    int opa_i = static_cast<int>(p->life * 200.0f);
    if (opa_i < 0)
        opa_i = 0;
    if (opa_i > 200)
        opa_i = 200;
    lv_obj_set_size(p->obj, sz, sz);
    lv_obj_set_pos(p->obj, static_cast<int>(p->x - sz * 0.5f),
                   static_cast<int>(p->y - sz * 0.5f));
    lv_obj_set_style_bg_color(p->obj, lv_color_hex(p->color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p->obj, static_cast<lv_opa_t>(opa_i), LV_PART_MAIN);
}

void RespawnParticle(Particle *p, bool birth_at_bottom)
{
    if (p == nullptr)
        return;
    const float spread = 220.0f;
    p->x = kChargeFxW * 0.5f + (ChargeRand01() - 0.5f) * spread;
    p->y = birth_at_bottom
               ? (kChargeFxH - 8.0f - ChargeRand01() * 28.0f)
               : (kChargeFxH * (0.30f + ChargeRand01() * 0.55f));
    p->vx = (ChargeRand01() - 0.5f) * 1.8f;
    p->vy = -(1.4f + ChargeRand01() * 3.0f);
    p->size = 3.0f + ChargeRand01() * 5.0f;
    p->life = 0.70f + ChargeRand01() * 0.30f;
    p->life_decay = 0.016f + ChargeRand01() * 0.020f;
    p->color = (ChargeRand() & 1u) ? kChargeBlueBright : kChargeBlueSoft;
    p->alive = true;
    ApplyParticleVisual(p);
}

void StopChargeEffect()
{
    if (s_ui.charge_stop_timer != nullptr) {
        lv_timer_delete(s_ui.charge_stop_timer);
        s_ui.charge_stop_timer = nullptr;
    }
    if (s_ui.charge_tick_timer != nullptr) {
        lv_timer_delete(s_ui.charge_tick_timer);
        s_ui.charge_tick_timer = nullptr;
    }
    if (s_ui.charge_defer_timer != nullptr) {
        lv_timer_delete(s_ui.charge_defer_timer);
        s_ui.charge_defer_timer = nullptr;
    }
    if (s_ui.charge_root != nullptr) {
        lv_obj_delete(s_ui.charge_root);
    }
    s_ui.charge_root = nullptr;
    s_ui.charge_tip = nullptr;
    for (Particle &p : s_ui.particles) {
        p = Particle{};
    }
    s_ui.charge_playing = false;
}

void OnChargeEffectTimeout(lv_timer_t * /*timer*/)
{
    s_ui.charge_stop_timer = nullptr;
    StopChargeEffect();
}

void OnParticleTick(lv_timer_t * /*timer*/)
{
    if (!s_ui.charge_playing || s_ui.charge_root == nullptr)
        return;

    for (Particle &p : s_ui.particles) {
        if (!p.alive || p.obj == nullptr)
            continue;

        p.x += p.vx;
        p.y += p.vy;
        p.vx *= 0.992f;
        p.vy -= 0.030f;
        p.life -= p.life_decay;

        if (p.life <= 0.0f || p.y < -10.0f) {
            RespawnParticle(&p, true);
            continue;
        }
        ApplyParticleVisual(&p);
    }
}

void StartChargeEffect(int battery_level)
{
    /* Disabled: any charge FX (particles / tip root) solid-blues the
     * flip-clock standby on this LVGL port. Keep clock only. */
    (void)battery_level;
    write(1, "SB_CHG_OFF\n", 11);
}

void MaybeTriggerChargeEffect(bool charging, int battery_level)
{
    /* Track charge edge only — do not start particle FX. */
    (void)battery_level;
    s_ui.last_charging = charging;
    s_ui.charge_primed = true;
}

void UpdateClockLabels()
{
    if (s_ui.date_lbl == nullptr || s_ui.clock_row == nullptr) return;

    time_t now = time(nullptr);
    struct tm tm_info = {};
    const bool have_time = localtime_r(&now, &tm_info) != nullptr &&
                           tm_info.tm_year >= 2025 - 1900;

    char digits[7] = "000000";
    if (have_time) {
        std::snprintf(digits, sizeof(digits), "%02d%02d%02d", tm_info.tm_hour,
                      tm_info.tm_min, tm_info.tm_sec);
        const int wday = tm_info.tm_wday;
        const char *weekday =
            (wday >= 0 && wday < 7) ? I18n::T(kWeekdayMsgIds[wday]) : "-";

        char date_str[64];
        std::snprintf(date_str, sizeof(date_str),
                      I18n::T("%04d年%02d月%02d日 星期%s"),
                      tm_info.tm_year + 1900, tm_info.tm_mon + 1,
                      tm_info.tm_mday, weekday);
        lv_label_set_text(s_ui.date_lbl, date_str);
    } else {
        lv_label_set_text(s_ui.date_lbl, I18n::T("----年--月--日 星期-"));
    }

    const bool animate = s_ui.clock_primed;
    for (int i = 0; i < kDigitCount; ++i) {
        FlipDigitSet(&s_ui.digits[i], digits[i], animate);
    }
    s_ui.clock_primed = true;

    int battery_level = 0;
    int charging = 0;
    int discharging = 0;
    if (metalio_board_get_battery(&battery_level, &charging, &discharging)) {
        if (battery_level < 0) battery_level = 0;
        if (battery_level > 100) battery_level = 100;
        MaybeTriggerChargeEffect(charging != 0, battery_level);
    }
}

void OnClockTimer(lv_timer_t * /*timer*/) { UpdateClockLabels(); }

void OnScreenUnloaded(lv_event_t * /*e*/)
{
    IdlePower_Detach(IdlePowerSession::Standby);
    StopChargeEffect();
    if (s_ui.update_timer != nullptr) {
        lv_timer_delete(s_ui.update_timer);
        s_ui.update_timer = nullptr;
    }
    for (int i = 0; i < kDigitCount; ++i) {
        lv_anim_delete(&s_ui.digits[i], nullptr);
    }
    std::memset(&s_ui, 0, sizeof(s_ui));
}

void OnStandbyLoaded()
{
    /* Match current charge state so UpdateClockLabels during CreateStatic
     * does not treat enter as a rising edge. Enter FX is deferred to
     * ScheduleChargeEffectAfterShow(); plug-in while already on standby
     * still fires via MaybeTriggerChargeEffect. */
    int battery_level = 0;
    int charging = 0;
    int discharging = 0;
    if (metalio_board_get_battery(&battery_level, &charging, &discharging)) {
        s_ui.last_charging = (charging != 0);
    } else {
        s_ui.last_charging = false;
    }
    s_ui.charge_primed = true;
}

void ScheduleChargeEffectAfterShow()
{
    /* Charge particle FX disabled — standby shows flip-clock only. */
    write(1, "SB_CHG_SKIP\n", 12);
}

void OnStandbyClicked(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    StandbyScreen::ReturnHome();
}

void standby_lifecycle_cb(screen_lifecycle_event_t event)
{
    PwrKey_OnScreenLifecycle("standby", event);
    StandbyScreen::LifecycleCallback(event);
}

}  // namespace

lv_obj_t *StandbyScreen::CreateStatic()
{
    write(1, "SB_CS0\n", sizeof("SB_CS0\n") - 1);
    /* Match ChatScreen: never leave orphan timers/anims from a prior enter. */
    if (s_ui.screen != nullptr || s_ui.update_timer != nullptr ||
        s_ui.charge_playing || s_ui.charge_defer_timer != nullptr) {
        write(1, "SB_CS_RESET\n", 12);
        StopChargeEffect();
        if (s_ui.update_timer != nullptr) {
            lv_timer_delete(s_ui.update_timer);
            s_ui.update_timer = nullptr;
        }
        for (int i = 0; i < kDigitCount; ++i) {
            lv_anim_delete(&s_ui.digits[i], nullptr);
        }
        std::memset(&s_ui, 0, sizeof(s_ui));
    }

    lv_obj_t *screen = lv_obj_create(NULL);
    /* Strip default theme (blue primary) before first paint — same as home. */
    lv_obj_remove_style_all(screen);
    screen_strip_obj_chrome(screen);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, OnStandbyClicked, LV_EVENT_CLICKED, nullptr);
    s_ui.screen = screen;

    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 28, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    s_ui.clock_row = CreateFlipClockRow(box);

    s_ui.date_lbl = lv_label_create(box);
    lv_label_set_text(s_ui.date_lbl, I18n::T("----年--月--日 星期-"));
    lv_obj_set_style_text_color(s_ui.date_lbl, lv_color_hex(0xE5E7EB),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_ui.date_lbl, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_ui.date_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_remove_flag(s_ui.date_lbl, LV_OBJ_FLAG_CLICKABLE);

    OnStandbyLoaded();
    UpdateClockLabels();
    write(1, "SB_CS_CLOCK\n", sizeof("SB_CS_CLOCK\n") - 1);
    s_ui.update_timer = lv_timer_create(OnClockTimer, 1000, nullptr);

    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    write(1, "SB_CS_END\n", sizeof("SB_CS_END\n") - 1);
    return screen;
}

lv_obj_t *StandbyScreen::Create()
{
    root_ = CreateStatic();
    return root_;
}

void StandbyScreen::LifecycleCallback(screen_lifecycle_event_t event)
{
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: standby_screen");
        IdlePower_Attach(IdlePowerSession::Standby, /*reset_activity=*/true);
        OnStandbyLoaded();
        UpdateClockLabels();
    } else {
        ESP_LOGI(TAG, "unload: standby_screen");
    }
}

void StandbyScreen::Show()
{
    write(1, "SB_SHOW\n", sizeof("SB_SHOW\n") - 1);
    lv_obj_t *old_scr = lv_screen_active();
    lv_obj_t *app = StandbyScreen::CreateStatic();
    write(1, "SB_CS_DONE\n", sizeof("SB_CS_DONE\n") - 1);
    screen_attach_lifecycle(app, standby_lifecycle_cb);
    lv_screen_load(app);
    write(1, "SB_LOADED\n", sizeof("SB_LOADED\n") - 1);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
    write(1, "SB_SHOW_END\n", sizeof("SB_SHOW_END\n") - 1);
    /* Claw4: play blue charge particles once after the screen is live. */
    ScheduleChargeEffectAfterShow();
}

void StandbyScreen::ReturnHome()
{
    IdlePower_NotifyActivity();
    write(1, "SB_HOME\n", 8);
    /* Must NOT call SwitchToHome() directly from LV_EVENT_CLICKED: that
     * sync-deletes the standby screen while it is still the event target,
     * which corrupts the LVGL heap on this port (solid blue frame).
     * Defer to lv_async_call so the click handler finishes first; then
     * SwitchToHome can safely free standby before recreating home. */
    if (lv_async_call(
            [](void * /*user_data*/) {
                write(1, "SB_HOME_GO\n", 10);
                HomeScreen::SwitchToHome();
            },
            nullptr) != LV_RESULT_OK)
    {
        write(1, "SB_HOME_FAIL\n", 12);
    }
}
