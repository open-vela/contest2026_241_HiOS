/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen utility functions — ported from MetalioClaw4
 * main/display/screen/screen_util.cc.
 */

#include "screen_util.h"
#include "standby_screen/standby_screen.h"
#include "idle_power_policy/idle_power_policy.h"
#include "system_chrome/system_chrome.h"
#include "system_chrome/control_center.h"
#include "system_chrome/recents_overlay.h"
#include "hi_input_method.h"

#include <cmath>
#include <algorithm>
#include <cstdlib>

#include "esp_log_shim.h"

namespace {
constexpr int16_t kSwipeBackThreshold = 100;
constexpr int16_t kSwipeEdgePx = 32;
constexpr int kDefaultEdgeTopInset = 90;
constexpr lv_obj_flag_t kSwipeBackIgnoreFlag = LV_OBJ_FLAG_USER_1;

struct SwipeBinding {
    lv_obj_t *screen;
    screen_swipe_back_cb_t callback;
    int top_inset;
    uint32_t generation;
    SwipeBinding *next;
    int edge_width = kSwipeEdgePx;
    int distance = kSwipeBackThreshold;
};
struct SwipeSession {
    lv_indev_t *input = nullptr;
    lv_obj_t *screen = nullptr;
    int x = 0;
    int y = 0;
    bool tracking = false;
    lv_obj_t *target = nullptr;
};
struct PendingBack {
    lv_obj_t *screen;
    screen_swipe_back_cb_t callback;
    uint32_t generation;
};
SwipeBinding *s_bindings;
SwipeSession s_swipe;
uint32_t s_binding_generation;
lv_timer_t *s_swipe_timer;

SwipeBinding *FindBinding(lv_obj_t *screen) {
    for (auto *b = s_bindings; b; b = b->next)
        if (b->screen == screen) return b;
    return nullptr;
}

bool OwnsHorizontalDrag(lv_obj_t *target, lv_obj_t *screen) {
    auto *obj = target;
    for (; obj && obj != screen; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_check_type(obj, &lv_slider_class) ||
            lv_obj_check_type(obj, &lv_arc_class) ||
            lv_obj_check_type(obj, &lv_roller_class) ||
            lv_obj_check_type(obj, &lv_textarea_class) ||
            lv_obj_check_type(obj, &lv_keyboard_class)) return true;
    }
    // Top-layer dialogs and keyboards own their pointer stream too.
    return obj != screen;
}

void InvokeBack(void *data) {
    auto *pending = static_cast<PendingBack *>(data);
    auto *binding = FindBinding(pending->screen);
    if (binding && binding->generation == pending->generation &&
        lv_screen_active() == pending->screen) pending->callback();
    std::free(pending);
}

void ProcessSwipe(lv_indev_t *input, lv_event_code_t code, lv_obj_t *event_target) {
    if (lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    if (code == LV_EVENT_PRESSED && !StandbyScreen::IsVisible())
        IdlePower_NotifyActivity();
    if (SystemChrome_ObservePointer(input, code, event_target)) {
        s_swipe = SwipeSession{};
        return;
    }
    auto *screen = lv_screen_active();
    auto *binding = FindBinding(screen);
    if (!binding || StandbyScreen::IsVisible() || ControlCenter_IsOpen() || Recents_IsOpen()) {
        s_swipe = SwipeSession{};
        return;
    }
    lv_point_t point;
    lv_indev_get_point(input, &point);
    if (code == LV_EVENT_PRESSED) {
        // LVGL can retarget a held pointer when it leaves a button. Keep
        // the original edge origin until release instead of restarting it.
        if (s_swipe.tracking && s_swipe.input == input && s_swipe.screen == screen) {
            s_swipe.target = event_target;
            return;
        }
        s_swipe = SwipeSession{};
        auto *target = event_target;
        if (point.x < 0 || point.x > binding->edge_width || point.y < binding->top_inset ||
            point.y >= lv_obj_get_height(screen) - 60 || OwnsHorizontalDrag(target, screen)) return;
        // No hit strip or synthetic press: the original control receives
        // its normal press, click and long-press events without delay.
        s_swipe.input = input;
        s_swipe.screen = screen;
        s_swipe.x = point.x;
        s_swipe.y = point.y;
        s_swipe.tracking = true;
        s_swipe.target = target;
        return;
    }
    if (s_swipe.input != input || s_swipe.screen != screen) return;
    if (code == LV_EVENT_DELETE || code == LV_EVENT_INDEV_RESET) {
        s_swipe = SwipeSession{};
        return;
    }
    if ((code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) || !s_swipe.tracking) return;
    const int dx = point.x - s_swipe.x;
    const int dy = std::abs(point.y - s_swipe.y);
    if (dx < binding->distance || dx < dy * 2) {
        if (code == LV_EVENT_RELEASED || dy > 40 || dx < -20)
            s_swipe = SwipeSession{};
        return;
    }
    lv_obj_t *target = s_swipe.target;
    s_swipe = SwipeSession{};
    auto *pending = static_cast<PendingBack *>(std::malloc(sizeof(PendingBack)));
    if (!pending) return;
    *pending = PendingBack{screen, binding->callback, binding->generation};
    if (lv_async_call(InvokeBack, pending) != LV_RESULT_OK) {
        std::free(pending);
        return;
    }
    // Commit once horizontal intent is clear, before scrolling or retargeting
    // can consume RELEASED. Ordinary taps and stationary holds remain intact.
    if (target && lv_obj_is_valid(target)) {
        lv_obj_remove_state(target, LV_STATE_PRESSED);
        lv_obj_send_event(target, LV_EVENT_PRESS_LOST, input);
    }
    lv_indev_reset(input, nullptr);
    lv_indev_wait_release(input);
}

void OnPointerEvent(lv_event_t *e) {
    auto *input = static_cast<lv_indev_t *>(lv_event_get_target(e));
    if (lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
    HiInputMethod_ObservePointer(e);
    ProcessSwipe(input, lv_event_get_code(e), static_cast<lv_obj_t *>(lv_event_get_param(e)));
}

void InstallPointerObserver() {
    if (!s_swipe_timer) {
        // LVGL sends PRESSING to widgets, not to input-device observers.
        // Sample the real held contact so retargeting and long holds cannot
        // swallow an edge swipe before RELEASED reaches the original widget.
        s_swipe_timer = lv_timer_create([](lv_timer_t *) {
            if (!s_swipe.tracking || !s_swipe.input) return;
            auto *input = s_swipe.input;
            if (lv_indev_get_state(input) != LV_INDEV_STATE_PRESSED) return;
            ProcessSwipe(input, LV_EVENT_PRESSING, s_swipe.target);
        }, 30, nullptr);
    }
    for (auto *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) continue;
        bool installed = false;
        for (uint32_t i = 0; i < lv_indev_get_event_count(input); ++i)
            if (lv_event_dsc_get_cb(lv_indev_get_event_dsc(input, i)) == OnPointerEvent)
                installed = true;
        if (!installed) lv_indev_add_event_cb(input, OnPointerEvent, LV_EVENT_ALL, nullptr);
    }
}

void OnSwipeScreenLoaded(lv_event_t *e) {
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) return;
    s_swipe = SwipeSession{};
    InstallPointerObserver();
}
void OnSwipeScreenDeleted(lv_event_t *e) {
    auto *screen = lv_event_get_current_target_obj(e);
    if (lv_event_get_target_obj(e) != screen) return;
    if (s_swipe.screen == screen) s_swipe = SwipeSession{};
    for (auto **slot = &s_bindings; *slot; slot = &(*slot)->next) {
        if ((*slot)->screen != screen) continue;
        auto *binding = *slot;
        *slot = binding->next;
        std::free(binding);
        break;
    }
}

void AttachSwipe(lv_obj_t *screen, int top_inset, screen_swipe_back_cb_t on_back) {
    if (!screen || !on_back) return;
    // Empty screen margins need an input target too. Children remain above
    // their root and continue to receive their own touches normally.
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    if (auto *binding = FindBinding(screen)) {
        if (top_inset < binding->top_inset) binding->top_inset = top_inset;
        InstallPointerObserver();
        return;
    }
    auto *binding = static_cast<SwipeBinding *>(std::malloc(sizeof(SwipeBinding)));
    if (!binding) return;
    *binding = SwipeBinding{screen, on_back, top_inset, ++s_binding_generation, s_bindings};
    s_bindings = binding;
    lv_obj_add_event_cb(screen, OnSwipeScreenLoaded, LV_EVENT_SCREEN_LOADED, nullptr);
    lv_obj_add_event_cb(screen, OnSwipeScreenDeleted, LV_EVENT_DELETE, nullptr);
    InstallPointerObserver();
}
}  // namespace

void screen_load_safe(lv_obj_t* scr) {
    if (scr == nullptr) {
        return;
    }
    InstallPointerObserver();
    if (lv_display_t* disp = lv_display_get_default()) {
        lv_display_enable_invalidation(disp, true);
    }
    lv_screen_load(scr);
    lv_obj_invalidate(scr);
}

void screen_make_input_passive(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_ADV_HITTEST);

    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        screen_make_input_passive(lv_obj_get_child(obj, i));
    }
}

void screen_strip_obj_chrome(lv_obj_t* obj) {
    if (obj == nullptr) return;
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

namespace {

void kill_chrome_selector(lv_obj_t* obj, lv_style_selector_t sel) {
    lv_obj_set_style_border_width(obj, 0, sel);
    lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, sel);
    lv_obj_set_style_outline_width(obj, 0, sel);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, sel);
    lv_obj_set_style_shadow_width(obj, 0, sel);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, sel);
}

}  // namespace

void screen_kill_theme_chrome(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    const lv_style_selector_t sels[] = {
        LV_PART_MAIN,
        LV_PART_ITEMS,
        LV_PART_INDICATOR,
        LV_PART_SCROLLBAR,
        static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED),
        static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_CHECKED),
        static_cast<lv_style_selector_t>(LV_PART_ITEMS | LV_STATE_CHECKED),
        static_cast<lv_style_selector_t>(LV_PART_ITEMS | LV_STATE_PRESSED),
    };
    for (lv_style_selector_t sel : sels) {
        kill_chrome_selector(obj, sel);
    }
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        screen_kill_theme_chrome(lv_obj_get_child(obj, i));
    }
}

void screen_swipe_back_ignore(lv_obj_t* obj, bool recursive) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, kSwipeBackIgnoreFlag);
    if (!recursive) {
        return;
    }
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        screen_swipe_back_ignore(lv_obj_get_child(obj, i), true);
    }
}

void screen_attach_swipe_back(lv_obj_t *scr, screen_swipe_back_cb_t on_back) {
    AttachSwipe(scr, kDefaultEdgeTopInset, on_back);
}

void screen_ensure_swipe_back(lv_obj_t *scr, screen_swipe_back_cb_t on_back) {
    AttachSwipe(scr, kDefaultEdgeTopInset, on_back);
}

void screen_attach_left_edge_back(lv_obj_t *scr, int top_inset,
                                  screen_swipe_back_cb_t on_back) {
    AttachSwipe(scr, top_inset < 0 ? 0 : top_inset, on_back);
}

void screen_set_back_sensitivity(lv_obj_t *scr, int edge_width, int distance) {
    if (auto *binding = FindBinding(scr)) {
        binding->edge_width = std::max(16, std::min(80, edge_width));
        binding->distance = std::max(48, std::min(160, distance));
    }
}

namespace {

void lifecycle_loaded_cb(lv_event_t* e) {
    InstallPointerObserver();
    auto cb = reinterpret_cast<screen_lifecycle_cb_t>(lv_event_get_user_data(e));
    if (cb != nullptr) {
        cb(SCREEN_LIFECYCLE_LOAD);
    }
    SystemChrome_Attach(lv_event_get_current_target_obj(e), false);
}

void lifecycle_unloaded_cb(lv_event_t* e) {
    auto cb = reinterpret_cast<screen_lifecycle_cb_t>(lv_event_get_user_data(e));
    if (cb != nullptr) {
        cb(SCREEN_LIFECYCLE_UNLOAD);
    }
}

}  // namespace

void screen_attach_lifecycle(lv_obj_t* scr, screen_lifecycle_cb_t cb) {
    if (scr == nullptr || cb == nullptr) {
        return;
    }
    void* user_data = reinterpret_cast<void*>(cb);
    lv_obj_add_event_cb(scr, lifecycle_loaded_cb, LV_EVENT_SCREEN_LOADED,
                        user_data);
    lv_obj_add_event_cb(scr, lifecycle_unloaded_cb, LV_EVENT_SCREEN_UNLOADED,
                        user_data);
    SystemChrome_Attach(scr, false);
}
