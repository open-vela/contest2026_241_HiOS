/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * ChatScreen — ported from MetalioClaw4
 * main/display/screen/chat_screen/chat_screen.h.
 *
 * Dark-theme chat page with two view modes:
 *   - Chat: left/right text bubbles (assistant/system left, user right)
 *   - Emotion: fullscreen centered emotion animation with bottom caption
 *     overlay. On ESP-IDF this uses the EAF vector animation widget
 *     (lv_eaf_create); on openvela/NuttX the EAF widget is not available,
 *     so the emotion is rendered as a large centered text label showing
 *     the emotion name. The caption bubble and all chat-bubble logic
 *     are ported verbatim.
 *
 * Messages / emotions are injected by LVAdapterDisplay. The header has
 * a back button, title/device-state, and a right-side menu
 * (emotion/chat/interrupt/clear).
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

#include <cstdint>

enum class ChatMsgDir : uint8_t {
    Left,   // assistant / system -> left dark-grey bubble
    Right,  // user                -> right dark-green bubble
};

class ChatScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "chat"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // Called by LVAdapterDisplay::SetChatMessage after acquiring the
    // esp_lv_adapter lock. No-op when the screen is not loaded.
    static void AddMessage(const char *text, ChatMsgDir dir);

    // Clear the message list ("清空" button / external call).
    static void ClearMessages();

    static bool IsActive();

    // Refresh the device-chat-state label next to the header title.
    // No-op when the screen is not loaded.
    static void RefreshDeviceState();

    // Switch the emotion animation by server emotion name. Falls back
    // to "neutral" for unsafe/missing names.
    static void SetEmotion(const char *emotion);
};
