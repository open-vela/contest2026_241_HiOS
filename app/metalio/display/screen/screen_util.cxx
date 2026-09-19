/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen utility functions — ported from MetalioClaw4
 * main/display/screen/screen_util.cc.
 */

#include "screen_util.h"

#include <cstdlib>

#include "esp_log_shim.h"

namespace {

// 720x720 panel -> use a slightly larger threshold than the 480p source.
constexpr int16_t kSwipeBackThreshold = 100;

struct SwipeState {
    int16_t start_x = 0;
    int16_t start_y = 0;
    bool tracking = false;
    bool from_edge = false;
};

/* Only treat right-swipes that begin near the left edge as "back". Mid-screen
 * horizontal noise (list scroll, icon press release after navigation) was
 * falsely leaving chat and stopping voice wake. */
constexpr int16_t kSwipeEdgePx = 36;

// One in-flight gesture at a time -- only one screen is loaded at any
// moment, so a single global is sufficient.
SwipeState s_swipe;

// Use an unused LVGL object flag bit to mark "this widget owns horizontal
// drag semantics; don't count its events as swipe-back candidates."  LVGL
// reserves USER_1..USER_4 for application use, which is exactly what we
// want here.
constexpr lv_obj_flag_t kSwipeBackIgnoreFlag = LV_OBJ_FLAG_USER_1;

// Walk from `from` up to `top` (exclusive) and return true if any ancestor
// (including `from`) is one of the widget types that intrinsically eat
// horizontal touch drags -- LVGL's built-in slider/arc/roller -- or has
// been explicitly marked by screen_swipe_back_ignore().
bool event_originates_in_drag_owner(lv_obj_t* from, lv_obj_t* top) {
    for (lv_obj_t* obj = from; obj != nullptr && obj != top;
         obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kSwipeBackIgnoreFlag)) {
            return true;
        }
        if (lv_obj_check_type(obj, &lv_slider_class) ||
            lv_obj_check_type(obj, &lv_arc_class) ||
            lv_obj_check_type(obj, &lv_roller_class)) {
            return true;
        }
    }
    return false;
}

/* Must not SwitchToHome()/delete the active screen from inside GESTURE or
 * RELEASED: the screen is still the event target, and sync-delete corrupts
 * the LVGL heap on this port (solid blue). Match StandbyScreen::ReturnHome. */
void invoke_swipe_back_cb(void* user_data) {
    auto on_back = reinterpret_cast<screen_swipe_back_cb_t>(user_data);
    if (on_back != nullptr) {
        on_back();
    }
}

void fire_swipe_back(screen_swipe_back_cb_t on_back, lv_indev_t* indev) {
    if (on_back == nullptr) {
        return;
    }
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    s_swipe.tracking = false;
    s_swipe.from_edge = false;
    if (lv_async_call(invoke_swipe_back_cb,
                      reinterpret_cast<void*>(on_back)) != LV_RESULT_OK) {
        on_back();
    }
}

void swipe_back_event_cb(lv_event_t* e) {
    auto on_back = reinterpret_cast<screen_swipe_back_cb_t>(
        lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);

    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }

    lv_obj_t* scr_obj = lv_event_get_current_target_obj(e);
    lv_obj_t* press_target = lv_event_get_target_obj(e);
    if (event_originates_in_drag_owner(press_target, scr_obj)) {
        s_swipe.tracking = false;
        s_swipe.from_edge = false;
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        s_swipe.start_x = point.x;
        s_swipe.start_y = point.y;
        s_swipe.from_edge = (point.x <= kSwipeEdgePx);
        s_swipe.tracking = s_swipe.from_edge;
        return;
    }

    // LVGL gesture path: only honor right-swipe that began on the left edge
    // AND traveled far enough — bare GESTURE without dx falsely left chat.
    if (code == LV_EVENT_GESTURE) {
        const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_RIGHT && s_swipe.from_edge && on_back != nullptr) {
            const int16_t dx = point.x - s_swipe.start_x;
            const int16_t dy = point.y - s_swipe.start_y;
            if (dx > kSwipeBackThreshold && std::abs(dy) < std::abs(dx)) {
                fire_swipe_back(on_back, indev);
            }
        }
        return;
    }

    // Manual press/release tracker for swipes that start on the bare
    // screen background, where LVGL's built-in gesture detector wouldn't
    // run (no widget was pressed).
    if (code != LV_EVENT_RELEASED || !s_swipe.tracking) {
        return;
    }

    s_swipe.tracking = false;
    const bool from_edge = s_swipe.from_edge;
    s_swipe.from_edge = false;
    if (!from_edge) {
        return;
    }

    const int16_t dx = point.x - s_swipe.start_x;
    const int16_t dy = point.y - s_swipe.start_y;
    if (dx > kSwipeBackThreshold && std::abs(dy) < std::abs(dx)) {
        fire_swipe_back(on_back, indev);
    }
}

// Recursively flag every descendant so LV_EVENT_GESTURE bubbles up to the
// screen, regardless of which widget was the press target.
void enable_gesture_bubble(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        enable_gesture_bubble(lv_obj_get_child(obj, i));
    }
}

void swipe_back_screen_loaded_cb(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_current_target_obj(e);
    enable_gesture_bubble(scr);
}

}  // namespace

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
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
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

void screen_attach_swipe_back(lv_obj_t* scr, screen_swipe_back_cb_t on_back) {
    if (scr == nullptr) return;
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    void* user_data = reinterpret_cast<void*>(on_back);
    lv_obj_add_event_cb(scr, swipe_back_event_cb, LV_EVENT_PRESSED,
                        user_data);
    lv_obj_add_event_cb(scr, swipe_back_event_cb, LV_EVENT_RELEASED,
                        user_data);
    lv_obj_add_event_cb(scr, swipe_back_event_cb, LV_EVENT_GESTURE,
                        user_data);
    lv_obj_add_event_cb(scr, swipe_back_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, nullptr);
}

namespace {

void lifecycle_loaded_cb(lv_event_t* e) {
    auto cb = reinterpret_cast<screen_lifecycle_cb_t>(lv_event_get_user_data(e));
    if (cb != nullptr) {
        cb(SCREEN_LIFECYCLE_LOAD);
    }
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
}
