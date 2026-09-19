/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device state event manager implementation — ported from
 * MetalioClaw4 main/device_state_event.cc.
 */

#include "device_state_event.h"

/* Global storage + placement-new singleton to avoid __cxa_guard_acquire
 * deadlock that may occur during early CRT startup on NuttX. */
static char g_dsem_storage[sizeof(DeviceStateEventManager)] alignas(DeviceStateEventManager);
static DeviceStateEventManager *g_dsem_ptr = nullptr;

DeviceStateEventManager &DeviceStateEventManager::GetInstance()
{
    if (!g_dsem_ptr)
    {
        g_dsem_ptr = new (g_dsem_storage) DeviceStateEventManager();
    }
    return *g_dsem_ptr;
}

void DeviceStateEventManager::RegisterStateChangeCallback(
    std::function<void(DeviceState, DeviceState)> callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(std::move(callback));
}

void DeviceStateEventManager::PostStateChangeEvent(
    DeviceState previous_state, DeviceState current_state)
{
    /* Take a snapshot of the callback list under the lock, then iterate
     * WITHOUT the lock so callbacks can safely call RegisterStateChangeCallback
     * (which would otherwise deadlock).
     *
     * The vector copy is now safe thanks to SBO (Small Buffer Optimisation)
     * in the std::function shim: small callables (lambdas capturing 1-3
     * pointers) are cloned inline without any heap allocation, eliminating
     * the heap fragmentation that previously caused corruption. */
    auto callbacks = GetCallbacks();
    for (size_t i = 0; i < callbacks.size(); ++i)
    {
        if (callbacks[i])
            callbacks[i](previous_state, current_state);
    }
}

std::vector<std::function<void(DeviceState, DeviceState)>>
DeviceStateEventManager::GetCallbacks()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return callbacks_;
}

DeviceStateEventManager::DeviceStateEventManager() = default;
DeviceStateEventManager::~DeviceStateEventManager() = default;
