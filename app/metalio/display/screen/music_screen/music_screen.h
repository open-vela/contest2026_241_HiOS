/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MusicScreen — ported from MetalioClaw4
 * main/display/screen/music_screen/music_screen.h.
 *
 * 720x720 蓝牙音乐播放器界面。原 ESP-IDF 版本通过 SimpleUart 把蓝牙模块
 * 切到模式三（AT+RX=1 / AT+MODE=3），然后注册 UART RX 回调解析手机回传
 * 的 JSON 数据流（song / lyrics / MPLAY / MPAUSE）。openvela 暂未暴露
 * SimpleUart，故 AT 命令与 UART RX 回调均以 stub 形式保留，UI 完整可用。
 *
 * 数据回调接口（PushSong / PushLyric / PushPlayState）允许上层把外部解析
 * 好的 song / lyric / play-state 推进去，替代原 UART RX 路径；连接
 * SimpleUart 后只需把 on_uart_data 转发到这几个静态方法即可。
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

#include <string>

class MusicScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "music"; }

    // Legacy static entry point — matches the original MetalioClaw4
    // signature used by HomeScreen / metalio_main.
    static lv_obj_t *CreateStatic();

    // Lifecycle hook (LOAD / UNLOAD). On LOAD the original switches the BT
    // module to music-receive mode 3 and registers a UART RX callback; on
    // UNLOAD it tears both down. The openvela port keeps the hook so the
    // home screen can wire it via screen_attach_lifecycle(); the BT mode
    // switch is stubbed.
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // ---- External data-push API (replaces UART RX callback) ---------------
    // Safe to call from any thread — internally marshals onto the LVGL
    // thread via lv_async_call(). No-op when the screen is not on stage.
    static void PushSong(const std::string &text);
    static void PushLyric(const std::string &text);
    static void PushPlayState(bool playing);
};
