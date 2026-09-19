/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * PwrKey handler — ported from MetalioClaw4
 * main/display/screen/pwr_key_handler.cc.
 *
 * The original registers click / long-press on the IOExpander PWR_KEY
 * pin. The NuttX board driver (metalio_pwr_key_init) polls the TCA9555
 * PWR_KEY input and invokes the two C-linkage trampolines below, which
 * forward to the dispatch logic (short-press -> standby / chat-toggle,
 * long-press -> power dialog). The screen stack is maintained so other
 * code can query PwrKey_ActiveScreen().
 */

#include "pwr_key_handler.h"

#include <cstring>

#include "esp_log_shim.h"
#include "lvgl.h"
#include "home_screen/home_screen.h"
#include "standby_screen/standby_screen.h"
#include "application.h"

#include <metalio/metalio.h>   /* metalio_pwr_key_init */

namespace {

constexpr const char *TAG = "PwrKey";
constexpr const char *kNoneScreen = "none";
constexpr const char *kHomeScreen = "home";
constexpr uint32_t kLongPressMs = 1500;
constexpr int kMaxStack = 8;

const char *s_stack[kMaxStack] = {};
int s_depth = 0;
bool s_inited = false;

bool IsChatToggleScreen(const char *name)
{
    return std::strcmp(name, "chat") == 0 ||
           std::strcmp(name, "digital_people") == 0;
}

void StackPush(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    if (s_depth < kMaxStack) {
        s_stack[s_depth++] = name;
    } else {
        s_stack[kMaxStack - 1] = name;
        ESP_LOGW(TAG, "screen stack full, replace top with %s", name);
    }
}

void StackRemoveTopmost(const char *name)
{
    if (name == nullptr || s_depth <= 0) {
        return;
    }
    for (int i = s_depth - 1; i >= 0; --i) {
        if (std::strcmp(s_stack[i], name) == 0) {
            for (int j = i; j < s_depth - 1; ++j) {
                s_stack[j] = s_stack[j + 1];
            }
            --s_depth;
            s_stack[s_depth] = nullptr;
            return;
        }
    }
}

const char *StackTop()
{
    return s_depth > 0 ? s_stack[s_depth - 1] : kNoneScreen;
}

void OnEnterStandbyAsync(void * /*arg*/) { StandbyScreen::Show(); }

void OnLeaveStandbyAsync(void * /*arg*/) { StandbyScreen::ReturnHome(); }

// Forward declaration of dispatchers (used by the platform key hook).
void OnShortPress();
void OnLongPress();

void OnShortPress()
{
    const char *screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "short-press on screen=%s (depth=%d)", screen, s_depth);

    // TODO(openvela): handle USB extend screen running check.

    if (std::strcmp(screen, kHomeScreen) == 0) {
        ESP_LOGI(TAG, "dispatch: enter standby_screen");
        lv_async_call(OnEnterStandbyAsync, nullptr);
        return;
    }

    if (std::strcmp(screen, "standby") == 0) {
        ESP_LOGI(TAG, "dispatch: leave standby -> home");
        lv_async_call(OnLeaveStandbyAsync, nullptr);
        return;
    }

    if (IsChatToggleScreen(screen)) {
        ESP_LOGI(TAG, "dispatch: ToggleChatState()");
        Application::GetInstance().ToggleChatState();
        return;
    }

    ESP_LOGI(TAG, "dispatch: no-op (screen has no short-press action)");
}

void OnLongPressAsync(void * /*arg*/)
{
    HomeScreen::ShowPowerOptionsDialog();
}

void OnLongPress()
{
    const char *screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "long-press %ums on screen=%s -> power dialog",
             static_cast<unsigned>(kLongPressMs), screen);
    lv_async_call(OnLongPressAsync, nullptr);
}

}  // namespace

/* C-linkage trampolines for the NuttX TCA9555 driver.  The board layer
 * runs a low-rate poll of the PWR_KEY input and invokes these callbacks
 * from its monitor thread; they forward to the LVGL dispatch logic above
 * (which itself only posts lv_async_call, never blocks). */
extern "C" void metalio_pwr_key_short_press_cb(void)
{
    OnShortPress();
}

extern "C" void metalio_pwr_key_long_press_cb(void)
{
    OnLongPress();
}

void PwrKey_Init()
{
    if (s_inited) {
        return;
    }

    int ret = metalio_pwr_key_init(metalio_pwr_key_short_press_cb,
                                   metalio_pwr_key_long_press_cb);
    if (ret != 0) {
        ESP_LOGW(TAG, "PwrKey_Init: metalio_pwr_key_init failed: %d", ret);
        return;
    }

    s_inited = true;
    s_depth = 0;
    ESP_LOGI(TAG,
             "armed: short-press + long-press %ums (active_screen=%s)",
             static_cast<unsigned>(kLongPressMs), PwrKey_ActiveScreen());
}

void PwrKey_OnScreenLifecycle(const char *name,
                              screen_lifecycle_event_t event)
{
    if (name == nullptr || name[0] == '\0') {
        name = kNoneScreen;
    }

    if (event == SCREEN_LIFECYCLE_LOAD) {
        StackPush(name);
        ESP_LOGD(TAG, "active_screen -> %s (load %s, depth=%d)", StackTop(),
                 name, s_depth);
        return;
    }

    StackRemoveTopmost(name);
    ESP_LOGD(TAG, "active_screen -> %s (unload %s, depth=%d)", StackTop(),
             name, s_depth);
}

const char *PwrKey_ActiveScreen()
{
    return StackTop();
}
