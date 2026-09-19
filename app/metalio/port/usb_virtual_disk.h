/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * UsbVirtualDisk — openvela stub for the MetalioClaw4 USB-MSC virtual
 * disk helper (originally in main/boards/common/usb_virtual_disk.h).
 *
 * The original switches GPIO24/25 from USB-Serial/JTAG to TinyUSB MSC
 * mode so a host PC can access the SD card as a mass-storage device.
 * openvela's TinyUSB integration is not yet wired into the board, so
 * this stub reports the feature as unsupported and no-ops every call.
 * The SD card screen guards all UI paths with IsSupported()/IsBusy()/
 * IsSdExportedToHost(), so an unsupported platform simply hides the
 * toggle button and shows "未支持" / disables deletion.
 *
 * Drop-in replacement: same public API, all state stays Idle/Disabled.
 */

#ifndef METALIO_USB_VIRTUAL_DISK_STUB_H
#define METALIO_USB_VIRTUAL_DISK_STUB_H

#include <functional>

class UsbVirtualDisk
{
public:
    enum class UiHint {
        Idle,
        Switching,
        Enabling,
        Disabling,
        EnabledHost,
        EnabledLocal,
        Disabled,
        EnableFailed,
        DisableFailed,
        NoSdCard,
        FormatRequired,
        HostBusy,
    };

    using UiNotifyFn = std::function<void()>;

    static UsbVirtualDisk &GetInstance()
    {
        static UsbVirtualDisk instance;
        return instance;
    }

    UsbVirtualDisk(const UsbVirtualDisk &) = delete;
    UsbVirtualDisk &operator=(const UsbVirtualDisk &) = delete;

    void  Init() {}
    bool  IsSupported() const { return false; }
    bool  IsGadgetActive() const { return false; }
    bool  IsBusy() const { return false; }
    bool  IsSdExportedToHost() const { return false; }
    UiHint GetUiHint() const { return UiHint::Disabled; }

    void  Toggle() {}
    void  DisableIfActive() {}
    void  SetUiNotify(UiNotifyFn /*fn*/) {}

    static const char *HintMsgid(UiHint /*hint*/)
    {
        return "USB MSC 未支持";
    }

private:
    UsbVirtualDisk() = default;
};

#endif  // METALIO_USB_VIRTUAL_DISK_STUB_H
