/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device state event manager — ported from
 * MetalioClaw4 main/device_state_event.h.
 *
 * Replaces the ESP-IDF event loop with a simple callback list. PostStateChange
 * dispatches synchronously to registered callbacks (callers should avoid
 * re-entrancy — the original posted to the ESP event loop and dispatched from
 * a separate task; in practice all callers run from Application's main loop).
 */

#ifndef METALIO_DEVICE_STATE_EVENT_H
#define METALIO_DEVICE_STATE_EVENT_H

#include "device_state.h"

#include <functional>
#include <vector>
#include <mutex>

enum
{
    XIAOZHI_STATE_CHANGED_EVENT,
};

struct device_state_event_data_t
{
    DeviceState previous_state;
    DeviceState current_state;
};

class DeviceStateEventManager
{
public:
    static DeviceStateEventManager &GetInstance();
    DeviceStateEventManager(const DeviceStateEventManager &) = delete;
    DeviceStateEventManager &operator=(const DeviceStateEventManager &) = delete;

    void RegisterStateChangeCallback(std::function<void(DeviceState, DeviceState)> callback);
    void PostStateChangeEvent(DeviceState previous_state, DeviceState current_state);
    std::vector<std::function<void(DeviceState, DeviceState)>> GetCallbacks();

private:
    DeviceStateEventManager();
    ~DeviceStateEventManager();

    std::vector<std::function<void(DeviceState, DeviceState)>> callbacks_;
    std::mutex mutex_;
};

#endif /* METALIO_DEVICE_STATE_EVENT_H */
