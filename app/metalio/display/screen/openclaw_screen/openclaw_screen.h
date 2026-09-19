/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenClawScreen — ported from MetalioClaw4
 * main/display/screen/openclaw_screen/openclaw_screen.h.
 *
 * 暗黑主题 OpenClaw 对话界面。进入先显示会话列表；点进某条会话进入
 * 详情页。详情页底部有「按住说话」按钮：按下录音、松开上传、上传成功
 * 后刷新消息列表。进入页面前检查设备是否已激活；未激活时弹出不可关闭
 * 的拦截弹窗（含返回按钮），背后页面内容保持可见但不可操作。
 *
 * 原实现直接调用 OpenClaw HTTP API（GET /devices/status、GET /conversation、
 * POST /upload、GET /conversation/<id>/messages、GET /conversation/removeAll、
 * GET /conversation/delete/<id>），并通过 Board::GetInstance().GetAudioCodec()
 * 检查 input_channels、Application::GetInstance().GetAudioService() 录音。
 * openvela 上：
 *   - HTTP 调用全部走 OpenClawHttpBackend 接口，默认
 *     StubOpenClawHttpBackend 返回空列表 / 失败，让 UI 流程可端到端跑通。
 *   - 录音路径保留（AudioService::ReadAudioData / EnableWakeWordDetection
 *     在 openvela 上可用），但 input_channels 检查省略（openvela 的 ES8311
 *     codec 是单声道）。
 *   - 设备激活检查走 Application::GetInstance().IsDeviceActivated()。
 *
 * UI 完全保留：会话列表页（header + 创建会话行 + 总数提示 + 会话条目）、
 * 详情页（header + 消息列表 + 状态文字 + 录音按钮）、激活拦截弹窗、清空
 * 确认弹窗、消息气泡（user 右 / assistant 左）。
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class OpenClawScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "openclaw"; }

    // Legacy static entry point — matches the original MetalioClaw4
    // signature used by HomeScreen / metalio_main.
    static lv_obj_t *CreateStatic();

    // Lifecycle hook (LOAD / UNLOAD). On LOAD the original checks device
    // activation; on UNLOAD it disables wake word detection (OpenClaw
    // does not own a voice-UI session). The openvela port keeps the hook
    // so the home screen can wire it via screen_attach_lifecycle().
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
