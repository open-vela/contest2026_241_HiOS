/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SimpleUart implementation — opens /dev/ttyS1 (Bluetooth audio codec UART2)
 * and reads it on a detached thread.
 */

#include "simple_uart.h"

#include "esp_log_shim.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <utility>   /* std::move */

static const char *TAG = "SimpleUart";

static constexpr const char *kDevPath = "/dev/ttyS1";

SimpleUart &SimpleUart::getInstance()
{
    static SimpleUart instance;
    return instance;
}

SimpleUart::~SimpleUart()
{
    if (fd_ >= 0)
    {
        close(fd_);
        fd_ = -1;
    }
}

bool SimpleUart::begin()
{
    if (initialized_)
        return true;

    fd_ = open(kDevPath, O_RDWR);
    if (fd_ < 0)
    {
        ESP_LOGE(TAG, "open %s failed: %d", kDevPath, errno);
        return false;
    }

    initialized_ = true;
    std::thread([this]() { rxLoop(); }).detach();
    ESP_LOGI(TAG, "%s opened, RX reader started", kDevPath);
    return true;
}

bool SimpleUart::isInitialized() const
{
    return initialized_;
}

bool SimpleUart::sendData(const uint8_t *data, size_t length)
{
    if (!initialized_ || fd_ < 0 || data == nullptr)
        return false;

    ssize_t n = write(fd_, data, length);
    return n == static_cast<ssize_t>(length);
}

bool SimpleUart::sendString(const char *data)
{
    if (data == nullptr)
        return false;
    return sendData(reinterpret_cast<const uint8_t *>(data), strlen(data));
}

bool SimpleUart::sendString(const std::string &data)
{
    return sendData(reinterpret_cast<const uint8_t *>(data.c_str()),
                    data.length());
}

void SimpleUart::registerCallback(
    std::function<void(const std::vector<uint8_t> &)> cb)
{
    std::lock_guard<std::mutex> lock(cb_mutex_);
    callback_ = std::move(cb);
}

void SimpleUart::rxLoop()
{
    uint8_t buf[1024];

    while (initialized_ && fd_ >= 0)
    {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int n = poll(&pfd, 1, 100);
        if (n <= 0)
            continue;

        if (!(pfd.revents & POLLIN))
            continue;

        ssize_t len = read(fd_, buf, sizeof(buf));
        if (len <= 0)
            continue;

        std::lock_guard<std::mutex> lock(cb_mutex_);
        if (callback_)
        {
            std::vector<uint8_t> data(buf, buf + len);
            callback_(data);
        }
    }
}
