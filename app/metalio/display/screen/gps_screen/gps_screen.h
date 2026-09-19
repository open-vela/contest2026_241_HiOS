/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * GpsScreen — ported from MetalioClaw4
 * main/display/screen/gps_screen/gps_screen.h.
 *
 * 卫星定位 App。展示 GpsService 解析出来的 GNSS 数据。
 *
 * 生命周期：GPS 模块靠 TCA9555 的 GPS_POWER 线供电；只在用户进入本屏幕
 * 期间通电，离开就断电（节流 + 避免常驻冷启动）。GpsService 自身从不
 * 触碰这条电源线 —— 这是 GpsScreen 的职责。
 *
 * openvela / NuttX 适配：
 *   - GpsService（NMEA 解析 + UART）→ stub：返回内置 demo snapshot，
 *     UI 立即有内容；接入真实 GNSS 驱动后替换 GpsService::Instance()。
 *   - Nt26Board / DualNetworkBoard（4G 模块 AT 指令）→ stub。
 *   - IOExpander::GPS_POWER → stub（NuttX IO 扩展器尚未端口化）。
 *   - HTTP /location/report/cell 与 static-map 下载 → stub：上报永远
 *     返回失败 + 友好提示，地图区域显示「请先获取定位」/「查询失败」。
 *   - SystemInfo::GetMacAddress / GetDeviceImei → 返回固定字符串。
 *   - Settings NVS → NuttX Settings（已存在的 ported Settings 类）。
 *   - esp_lv_adapter_lock / unlock → lv_lock / lv_unlock。
 *   - FreeRTOS xTaskCreate / portMUX → freertos_shim.h（mutex 用
 *     std::mutex，比 portMUX_TYPE 跨平台更稳）。
 *   - <cmath> 不使用（NuttX libc 缺 math.h）；用 __builtin_* 内置。
 *   - LVGL widget 构建（tabview / 信息卡 / 工具栏 / 地图窗口 / 模拟
 *     定位弹框 / 缩放控件）原样保留。
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class GpsScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "gps"; }

    // Legacy static entry point — used by HomeScreen grid launcher /
    // ScreenManager wiring (matches the MetalioClaw4 API).
    static lv_obj_t *CreateStatic();

    // Screen lifecycle hook (LOAD / UNLOAD).
    // On LOAD we would power up the GPS module; on UNLOAD we would power it
    // down. Both are stubbed on openvela until the IO-expander driver lands.
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
