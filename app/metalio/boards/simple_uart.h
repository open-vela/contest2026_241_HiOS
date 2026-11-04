/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SimpleUart — NuttX equivalent of the MetalioClaw4 SimpleUart helper.
 *
 * The original (third_party/MetalioClaw4/main/boards/common/SimpleUart.hpp)
 * wraps ESP-IDF's UART driver to talk to the external Bluetooth audio codec
 * (CX25601N) over UART2. On NuttX that UART is a plain /dev/ttyS1 character
 * device, so this class just opens the device, sends bytes, and runs a
 * background reader thread that dispatches received chunks to a registered
 * callback (mirroring the original registerCallback semantics).
 */

#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class SimpleUart
{
public:
    static SimpleUart &getInstance();

    SimpleUart(const SimpleUart &) = delete;
    SimpleUart &operator=(const SimpleUart &) = delete;

    /* Open /dev/ttyS1 and start the background RX reader. Idempotent. */
    bool begin();

    bool isInitialized() const;

    bool sendData(const uint8_t *data, size_t length);
    bool sendString(const char *data);
    bool sendString(const std::string &data);
    /* Worker-only: wait for UART ownership and finish the whole command. */
    bool sendStringWait(const std::string &data, unsigned timeout_ms = 6000, std::function<bool()> valid = {});
    // Worker-only: own both writes and the mode ACK as one transaction.
    bool sendModeWait(int mode, std::function<bool()> valid = {});

    void registerCallback(std::function<void(const std::vector<uint8_t> &)> cb);
    void registerAudioObserver(std::function<void(const std::vector<uint8_t> &)> cb);

private:
    SimpleUart() = default;
    ~SimpleUart();

    void rxLoop();
    void DispatchReceived(const std::vector<uint8_t>& data);

    int  fd_ = -1;
    std::atomic<bool> initialized_{false};
    std::mutex init_mutex_;
    std::mutex cb_mutex_;
    std::function<void(const std::vector<uint8_t> &)> callback_,audio_observer_;
};
