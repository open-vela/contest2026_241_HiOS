/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * EspClawScreen — local "edge agent" emote digital-human, reimplemented
 * natively for openvela.
 *
 * In the reference firmware ESPClaw is a separate ESP-IDF binary
 * (edge_agent) that switches the boot partition to ota_1 and runs a
 * local emotion/emote agent on top of the esp_emote_gfx engine.  That
 * binary and its engine source are not shipped in this repo, so openvela
 * reimplements the same observable behaviour in LVGL and drives the
 * conversation through the existing cloud link (OpenClaw / MQTT / WS)
 * plus AudioService wake-word + ASR.
 *
 * The screen mirrors the original EmoteDisplay contract:
 *   SetEmotion(category)  -> switch the central emotion face
 *   SetStatus(text)       -> transient status / toast
 *   SetChatMessage(role, content) -> user / system chat bubble
 * Device state (idle / connecting / listening / speaking) is polled from
 * Application and drives a mic / speaker indicator plus status text.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class EspClawScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "espclaw"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // Emote-agent state API (matches the original edge_agent EmoteDisplay).
    // Safe to call while the screen is not on stage: the value is cached
    // and applied on the next Create().
    static void SetEmotion(const char *category);
    static void SetStatus(const char *text);
    static void SetChatMessage(const char *role, const char *content);
    static bool IsActive();
};
