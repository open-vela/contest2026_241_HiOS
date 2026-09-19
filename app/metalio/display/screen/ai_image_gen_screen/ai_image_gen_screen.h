/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * AiImageGenScreen — ported from MetalioClaw4
 * main/display/screen/ai_image_gen_screen/ai_image_gen_screen.h.
 *
 * AI voice-to-image page.
 *
 * Flow: press-and-hold to talk -> POST /xiaozhi/api/asr?format=wav to
 * get the prompt -> POST /xiaozhi/api/dashscope/text2image (n=1..4) ->
 * poll GET .../text2image/tasks/{taskId}?maxSide=500 -> download the
 * imageUrls and display them on the page.
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
