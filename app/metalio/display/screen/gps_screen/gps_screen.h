/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * GpsScreen — ported from MetalioClaw4
 * main/display/screen/gps_screen/gps_screen.h.
 *
 * 卫星定位 App。展示 GpsService 解析出来的 GNSS 数据。
 *
 * HiOS: reads board GNSS snapshots and NT26 WiFi/cell scans. Network
 * location uses beaconDB directly; Photon reverse geocoding and OSM tiles
 * are also fetched by the device. RGB565 maps are composed locally and
 * compressed tiles are cached for seven days on the SD card.
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
    // Board bringup owns the GNSS reader; this hook follows screen state.
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
