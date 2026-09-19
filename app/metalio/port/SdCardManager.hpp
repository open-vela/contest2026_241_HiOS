/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SdCardManager — openvela stub for the MetalioClaw4 board-level SDMMC
 * singleton (originally in main/boards/common/SdCardManager.hpp).
 *
 * The original wraps ESP-IDF's esp_vfs_fat_sdmmc_* + sdmmc_host APIs and
 * exposes Mount()/Unmount()/InitRawCardForMsc()/GetCard()/IsMounted()/
 * GetMountPoint(). openvela's NuttX SDHCI driver mounts the filesystem
 * at board init time via fs_mount, so the app does not need to drive the
 * mount lifecycle itself — it only needs to query whether the volume is
 * available and where it lives.
 *
 * This stub provides the same public surface with no-op semantics:
 *   - Mount() returns false (the board is responsible for mounting).
 *   - IsMounted() returns mounted_ which the board bring-up code can set
 *     via SetMounted() once NuttX's /dev/mmcsd0 is mounted at /sdcard.
 *   - GetCard() returns nullptr (no sdmmc_card_t equivalent on NuttX).
 *   - GetMountPoint() returns "/sdcard" (the conventional NuttX path).
 *
 * Drop-in replacement: the screen code calls IsMounted()/GetMountPoint()
 * only; the MSC raw-card paths are not invoked from the UI layer.
 */

#ifndef METALIO_SD_CARD_MANAGER_STUB_HPP
#define METALIO_SD_CARD_MANAGER_STUB_HPP

#include <cstdint>
extern "C" {
#include <metalio/metalio.h>
}

class SdCardManager
{
public:
    static constexpr const char *kMountPoint = "/sdcard";

    static SdCardManager &GetInstance()
    {
        static SdCardManager instance;
        return instance;
    }

    SdCardManager(const SdCardManager &) = delete;
    SdCardManager &operator=(const SdCardManager &) = delete;

    // Prefer metalio_sdcard_mount_ro(); keep API parity for screen code.
    bool Mount() { return metalio_sdcard_mount_ro() == 0; }
    void Unmount() {}

    // MSC raw-card paths — not supported on openvela yet.
    void *InitRawCardForMsc() { return nullptr; }
    void  ReleaseRawCard() {}
    void  NotifyExternalAppMount(void * /*card*/) {}
    void  NotifyExportedToHost() {}
    bool  RemountVfsFromCard() { return metalio_sdcard_is_mounted(); }

    // FAT mount is deferred past bringup (see metalio_sdcard_mount_ro).
    bool IsMounted() const { return metalio_sdcard_is_mounted(); }

    void        *GetCard() const { return nullptr; }
    bool         HasCard() const { return IsMounted(); }
    const char  *GetMountPoint() const { return kMountPoint; }

    // Retained for API parity; prefer metalio_sdcard_mount_ro().
    void SetMounted(bool mounted) { (void)mounted; }

private:
    SdCardManager() = default;
};

#endif  // METALIO_SD_CARD_MANAGER_STUB_HPP
