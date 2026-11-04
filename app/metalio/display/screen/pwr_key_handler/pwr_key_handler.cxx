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
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <pthread.h>

#include <nuttx/timers/pwm.h>

#include "esp_log_shim.h"
#include "lvgl.h"
#include "home_screen/home_screen.h"
#include "standby_screen/standby_screen.h"
#include "application.h"
#include "system_chrome/control_center.h"   /* PulseVibrateAsync */
#include "idle_power_policy/idle_power_policy.h"

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
    IdlePower_NotifyActivity();
    const char *screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "short-press on screen=%s (depth=%d)", screen, s_depth);

    if (std::strcmp(screen, "standby") == 0)
        StandbyScreen::ReturnHome();
    else
        StandbyScreen::Show();
}

void OnLongPressAsync(void * /*arg*/)
{
    HomeScreen::ShowPowerOptionsDialog();
}

void OnLongPress()
{
    IdlePower_NotifyActivity();
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
    lv_lock();
    lv_async_call([](void *) { OnShortPress(); }, nullptr);
    lv_unlock();
}

extern "C" void metalio_pwr_key_long_press_cb(void)
{
    lv_lock();
    lv_async_call([](void *) { OnLongPress(); }, nullptr);
    lv_unlock();
}

extern "C" void metalio_pwr_key_press_cb(void)
{
    /* Lock (home → standby) and unlock (standby → home) stay silent.
     * The press edge is the only haptic on this key, and it fires before
     * short vs long is known, so both of those screens skip the motor. */
    const char *screen = PwrKey_ActiveScreen();
    if (std::strcmp(screen, kHomeScreen) == 0 ||
        std::strcmp(screen, "standby") == 0) {
        return;
    }

    /* Board already pulses once at power-on. Suppress press-edge haptic
     * for a short boot window so a held/bouncing PWR_KEY cannot fire a
     * second motor buzz while the boot guide is coming up. */
    static bool s_boot_gate = true;
    static int64_t s_gate_start_ms = 0;
    if (s_boot_gate) {
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const int64_t now_ms =
            static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        if (s_gate_start_ms == 0) {
            s_gate_start_ms = now_ms;
        }
        if (now_ms - s_gate_start_ms < 800) {
            return;
        }
        s_boot_gate = false;
    }
    /* 80 ms short pulse — start the motor synchronously so the buzz is felt
     * the instant the key edge is detected (no thread-spawn latency). */
    PulseVibrateInstant(80 * 1000);
}

void PwrKey_Init()
{
    if (s_inited) {
        return;
    }

    int ret = metalio_pwr_key_init(metalio_pwr_key_short_press_cb,
                                   metalio_pwr_key_long_press_cb,
                                   metalio_pwr_key_press_cb);
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
