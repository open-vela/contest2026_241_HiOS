/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SdCardScreen — ported from MetalioClaw4
 * main/display/screen/sd_card_screen/sd_card_screen.h.
 *
 * SD card browser app.
 *   - Top: back button + title
 *   - Status row: detection dot + "已插入 / 未检测到"
 *   - Capacity line: "剩余 X / 总容量 Y" (only when mounted)
 *   - Virtual-U-disk toggle (hidden / disabled when UsbVirtualDisk is
 *     unsupported — which is always true on openvela today)
 *   - Path label + scrollable file list, navigates into subdirectories
 *   - Tap a jpg/png/sjpg -> full-screen image preview; tap txt -> text
 *     preview (scrollable); preview back button closes the overlay
 *   - Per-row delete button (directories are not deletable)
 *
 * The original relies on SdCardManager (ESP-IDF sdmmc + fatfs) and
 * UsbVirtualDisk (TinyUSB MSC). Both are stubbed in apps/metalio/port/:
 * SdCardManager reports mounted_/GetMountPoint() (board bring-up calls
 * SetMounted(true) once NuttX mounts /dev/mmcsd0 at /sdcard), and
 * UsbVirtualDisk reports unsupported, hiding the toggle.
 *
 * File operations use POSIX (opendir/readdir/stat/unlink/fopen) which
 * NuttX provides natively via its VFS. LVGL image loads use the "S:"
 * POSIX driver letter (lv_fs_posix) — wired up by the LVGL NuttX
 * integration in lv_conf_internal.h.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class SdCardScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "sd_card"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
