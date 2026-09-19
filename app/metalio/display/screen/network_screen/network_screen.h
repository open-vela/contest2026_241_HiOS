/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NetworkScreen — ported from MetalioClaw4
 * main/display/screen/network_screen/network_screen.h.
 *
 * 720x720 网络配置界面：三个 Tab —— 附近 WiFi / 已保存 WiFi / 网络切换
 * （4G 模式下还会出现 SIM 卡切换 Tab）。
 *
 * 原实现直接驱动 ESP-IDF 的 esp_wifi 栈做扫描 / 连接，并通过
 * SsidManager 把已连接过的网络写入 NVS，再通过 DualNetworkBoard /
 * Nt26Board 切换 WiFi <-> 4G 与 SIM 卡槽位。openvela 上：
 *   - esp_wifi_* API 由 NetworkScreenBackend 接口封装，默认
 *     StubNetworkScreenBackend 返回空扫描结果 / 连接失败。
 *   - SsidManager / DualNetworkBoard / Nt26Board 同样走 backend 钩子。
 *   - 重启走 Application::GetInstance().Reboot()。
 * UI 与原版完全一致：tabview、附近 WiFi 列表（含 RSSI / 加密标记）、
 * 密码输入弹窗（textarea + 内嵌键盘）、连接进度 / 成功 / 失败模态、
 * 网络切换 / SIM 切换按钮 + 重启倒计时遮罩。
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class NetworkScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "network"; }

    // Legacy static entry point — matches the original MetalioClaw4
    // signature used by HomeScreen / metalio_main.
    static lv_obj_t *CreateStatic();

    // Lifecycle hook (LOAD / UNLOAD). On LOAD the original starts the
    // local STA stack and refreshes the saved-SIM / saved-network lists;
    // on UNLOAD it tears the STA stack down. The openvela port keeps the
    // hook so the home screen can wire it via screen_attach_lifecycle();
    // the wifi stack start/stop is stubbed.
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
