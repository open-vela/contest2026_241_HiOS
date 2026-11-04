/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * AiImageGenScreen — ported from MetalioClaw4
 * main/display/screen/ai_image_gen_screen/ai_image_gen_screen.h.
 *
 * Direct AI Horde image page: text -> HTTPS generate/async -> check ->
 * status -> HTTPS image download -> bounded on-device WebP decoding.
 * Anonymous access, 512px output, 1-4 images. No LAN adapter is used.
 * Optional imagegen/asr_url enables voice using a compatible cloud ASR.
 * Without it, the main button opens the system text composer.
 *
 * Recording / HTTP / polling all run on a dedicated FreeRTOS worker
 * task (via freertos_shim) so the LVGL thread is never blocked.
 *
 * Porting notes (openvela/NuttX):
 *   - esp_log.h               -> esp_log_shim.h
 *   - esp_timer.h             -> esp_timer_shim.h (esp_timer_get_time)
 *   - esp_lv_adapter.h        -> removed; esp_lv_adapter_lock/unlock
 *                                replaced with lv_lock()/lv_unlock().
 *   - esp_heap_caps.h         -> removed; heap_caps_malloc/free replaced
 *                                with malloc/free (NuttX has no PSRAM
 *                                pool allocator; the standard heap is
 *                                used). The HeapCapsDeleter / CapsBuffer
 *                                helper types are preserved so the rest
 *                                of the buffer-management code is
 *                                unchanged.
 *   - freertos/FreeRTOS.h + task.h -> freertos_shim.h (provides
 *                                xTaskCreate / vTaskDelete / TaskHandle_t).
 *                                xTaskGetCurrentTaskHandle() is provided
 *                                by a local inline that calls getpid().
 *   - cJSON.h                 -> cJSON_compat.h (NuttX cJSON library).
 *   - board.h                 -> board_shim.h.
 *   - http.h                  -> ota.h (BSD-socket Http class).
 *                                Board::GetNetwork()->CreateHttp(0)
 *                                becomes a plain Http instance.
 *   - HomeScreen::Create()    -> HomeScreen::CreateStatic().
 *   - LvglAllocatedImage / lvgl_image.h already ported.
 *   - Class now inherits from Screen and overrides Create() / Name()
 *      while preserving the original static API (Create /
 *      LifecycleCallback).
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class AiImageGenScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "ai_image_gen"; }

    // Legacy static entry point — used by home_screen / metalio_main
    // callers that still use the original MetalioClaw4 signature.
    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
