/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>

namespace PhonePcm {

// Board adapters normalize their wire format to signed 16-bit mono samples.
// Open must acquire exclusive ownership and verify the actual hardware route.
// Read/Write return a sample count, zero on timeout, or a negative errno.
// Each operation must honor timeout_ms. Cancel wakes pending I/O without
// freeing DMA/storage; Close releases it after every bridge thread has joined.
// Adapters and contexts must outlive Stop(). The two ports must be distinct.
struct Port {
    void *context = nullptr;
    uint32_t sample_rate = 0;
    int (*open)(void *) = nullptr;
    int (*read)(void *, int16_t *, size_t, unsigned timeout_ms) = nullptr;
    int (*write)(void *, const int16_t *, size_t, unsigned timeout_ms) = nullptr;
    void (*cancel)(void *) = nullptr;
    void (*close)(void *) = nullptr;
};

enum class State { Idle, Starting, Running, Fault };

struct Statistics {
    uint32_t modem_input = 0;
    uint32_t headset_output = 0;
    uint32_t headset_input = 0;
    uint32_t modem_output = 0;
};

// Transport only: does not select GPIOs, send modem AT commands, switch the
// amplifier, stop AI audio or alter Bluetooth profiles. A board/phone route
// owner must coordinate those operations before opening the two adapters.
// Call Start/Stop from a control worker, never from the UI or an I/O callback.
class Bridge {
public:
    Bridge();
    ~Bridge();
    Bridge(const Bridge &) = delete;
    Bridge &operator=(const Bridge &) = delete;

    // Starts only after both directions have transferred real PCM samples.
    // Failure closes both adapters and leaves the existing route to its owner.
    int Start(const Port &modem, const Port &headset);
    int Stop();
    State GetState();
    int LastError();
    Statistics GetStatistics();

private:
    struct Session;
    pthread_mutex_t control_ = PTHREAD_MUTEX_INITIALIZER;
    Session *session_ = nullptr;
    int last_error_ = 0;
    Statistics last_statistics_{};
    int StopLocked();
};

} // namespace PhonePcm
