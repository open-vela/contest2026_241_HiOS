/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * RecordingScreen — ported from MetalioClaw4
 * main/display/screen/recording_screen/recording_screen.h.
 *
 * 录音 App（依赖 SD 卡）:
 *   - 进入前检查 SdCardManager 是否已挂载；无卡则仅显示提示，无法录音/列表
 *   - Tab「录音」: 开始/结束录音，显示计时，保存 Ogg Opus 到 /sdcard/recordings/
 *   - Tab「列表」: 列出录音；点击进入详情（播放 / 转写），可删除
 *   - 详情页: 播放、调用 /api/v1/asr/transcribe 转写并展示全文/对话/摘要
 *
 * Porting notes:
 *   - AudioService is wired through Application::GetInstance().GetAudioService()
 *     and is the real ported NuttX audio service (Opus encode/decode are
 *     currently stubbed inside AudioService itself, so Opus files contain
 *     raw PCM passthrough — to be upgraded when libopus is linked).
 *   - HTTP uploads (ASR transcribe / audio-records query) are stubbed via
 *     a small RecordingHttpBackend interface; the default
 *     StubRecordingHttpBackend returns failure so the UI flow remains
 *     intact without a network stack.
 *   - Lifecycle: UNLOAD stops recording / playback / ASR tasks and
 *     restores the wake word if this screen disabled it.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class RecordingScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "recording"; }

    // Legacy static entry point — matches the MetalioClaw4 signature
    // (used by HomeScreen grid launcher / ScreenManager wiring).
    static lv_obj_t *CreateStatic();

    // Screen lifecycle hook (LOAD / UNLOAD).
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
