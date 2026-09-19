/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttxLvglDisplay — concrete LvglDisplay subclass for openvela/NuttX.
 *
 * This is the openvela equivalent of MetalioClaw4's LVAdapterDisplay.
 * It connects the application-level Display wrapper to the NuttX LVGL
 * framebuffer driver (/dev/fb0) and touchscreen (/dev/input0), starts
 * the LVGL event loop in a dedicated thread, and creates the boot →
 * home screen transition.
 *
 * Thread safety: CONFIG_LV_OS_PTHREAD=y makes lv_lock()/lv_unlock() a real
 * recursive mutex.  Lock()/Unlock() delegate to lv_lock()/lv_unlock() so the
 * render thread, Application-thread DisplayLockGuard holders, and per-screen
 * worker threads all serialise on the same LVGL lock.
 */

#ifndef METALIO_NUTTX_LVGL_DISPLAY_H
#define METALIO_NUTTX_LVGL_DISPLAY_H

#include "lvgl_display.h"

#include <lvgl.h>
#include <pthread.h>
#include <atomic>
#include <chrono>

class NuttxLvglDisplay : public LvglDisplay
{
public:
    NuttxLvglDisplay();
    virtual ~NuttxLvglDisplay();

    /* Create the boot screen and schedule the transition to HomeScreen
     * after a short delay (mirrors LVAdapterDisplay::SetupUI).          */
    void SetupUI();

    /* Start the LVGL event loop thread.  Must be called AFTER board
     * init completes (from Application::Start()) to avoid stack/heap
     * races with the ESP_LOGI/write() calls in the init path.          */
    void StartLvglThread();
    void StartDisplayThread() override { StartLvglThread(); }
    void ArmHomeTransition() override;
    bool IsHomeReady() const override;
    /* Display is active only if LVGL disp handle is valid */
    bool IsActive() const override { return display_ != nullptr; }

    /* Route Xiaozhi TTS/STT/emotion into ChatScreen / DigitalPeopleScreen
     * (same policy as MetalioClaw4 LVAdapterDisplay). */
    void SetEmotion(const char *emotion) override;
    void SetChatMessage(const char *role, const char *content) override;
    /* Match MetalioClaw4 LVAdapterDisplay — overlay status bar is unused;
     * LvglDisplay's status_label_ path corrupts LVGL on this port (blue). */
    void SetStatus(const char *status) override;
    void ShowNotification(const char *notification, int duration_ms) override;
    void UpdateStatusBar(bool update_all = false) override;

protected:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

private:
    pthread_t lvgl_thread_;
    bool lvgl_thread_created_{false};
    std::atomic<bool> lvgl_running_{false};

    void LvglEventLoop();
    static void *LvglEventLoopEntry(void *arg);
};

#endif /* METALIO_NUTTX_LVGL_DISPLAY_H */
