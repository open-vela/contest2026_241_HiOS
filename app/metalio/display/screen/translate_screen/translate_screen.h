/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * TranslateScreen — ported from MetalioClaw4
 * main/display/screen/translate_screen/translate_screen.h.
 *
 * Sonicloud real-time simultaneous interpretation page.
 *
 * Flow: pick source / target language -> tap "实时翻译" ->
 *   POST /xiaozhi/api/sinicloud/token to obtain a wsUrl ->
 *   WebSocket streams 16 kHz PCM, the page shows the recognised source
 *   text and the translation. The protocol follows open.sinicloud.com
 *   real-time speech-stream ASR docs; the implementation mirrors the
 *   translate-test web demo.
 *
 * Recording / HTTP / WebSocket all run on a dedicated FreeRTOS worker
 * task (via freertos_shim) so the LVGL thread is never blocked.
 *
 * Porting notes (openvela/NuttX):
 *   - esp_log.h               -> esp_log_shim.h
 *   - esp_lv_adapter.h        -> removed; esp_lv_adapter_lock/unlock
 *                                replaced with lv_lock()/lv_unlock().
 *   - freertos/FreeRTOS.h     -> freertos_shim.h (task.h pulled in).
 *   - cJSON.h                 -> cJSON_compat.h (NuttX cJSON library).
 *   - board.h                 -> board_shim.h (Board singleton).
 *   - http.h                  -> ota/ota.h (BSD-socket Http class).
 *                                Board::GetNetwork()->CreateHttp(0)
 *                                becomes a plain `Http` instance.
 *   - <web_socket.h>          -> websocket_protocol.h (BSD-socket
 *                                WebSocket). ws->Close() -> ws->Stop();
 *                                SetReceiveBufferSize / OnError are
 *                                not provided by the NuttX WebSocket
 *                                and are stubbed with TODOs.
 *   - HomeScreen::Create()    -> HomeScreen::CreateStatic().
 *   - Class now inherits from Screen and overrides Create() / Name()
 *      while preserving the original static API (Create / LifecycleCallback).
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class TranslateScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "translate"; }

    // Legacy static entry point — used by home_screen / metalio_main
    // callers that still use the original MetalioClaw4 signature.
    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
