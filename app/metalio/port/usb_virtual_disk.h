/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
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

    void Init();
    bool IsSupported() const;
    bool IsGadgetActive() const;
    bool IsBusy() const;
    bool IsSdExportedToHost() const;
    UiHint GetUiHint() const;

    void Toggle();
    bool StartDisplay();
    void StopDisplay();
    bool IsDisplayActive() const;
    bool IsDisplayRequested() const;
    void DisableIfActive();
    void SetUiNotify(UiNotifyFn fn);

    static const char *HintMsgid(UiHint hint);

private:
    UsbVirtualDisk() = default;
};

