#include "hi_task_cpp.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * SimpleUart implementation — opens /dev/ttyS1 (Bluetooth audio codec UART2)
 * and reads it on a detached thread.
 */

#include "simple_uart.h"

#include "esp_log_shim.h"
#include <metalio/metalio.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <algorithm>
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
    // After open, init_mutex_ also serializes RX dispatch. Callbacks can
    // request begin() again and must use this lock-free initialized path.
    if(initialized_)return true;
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (initialized_)
        return true;

    fd_ = open(kDevPath, O_RDWR | O_NONBLOCK);
    if (fd_ < 0)
    {
        ESP_LOGE(TAG, "open %s failed: %d", kDevPath, errno);
        return false;
    }

    initialized_ = true;
    HiTask::LegacyThread([this]() { rxLoop(); }).detach();
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

    // UI commands fail promptly while a mode transaction owns the UART.
    if (!metalio_bt_is_powered() || metalio_bt_transport_trylock() < 0)
        return false;
    ssize_t n = write(fd_, data, length);
    metalio_bt_transport_unlock();
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

bool SimpleUart::sendStringWait(const std::string &data,unsigned timeout_ms,std::function<bool()> valid)
{
    if(!initialized_||fd_<0||data.empty()||(valid&&!valid()))return false;
    unsigned waited=0;
    while(metalio_bt_transport_trylock()<0){
        if(waited>=timeout_ms||!metalio_bt_is_powered()||(valid&&!valid()))return false;
        usleep(10000);waited+=10;
    }
    if(valid&&!valid()){metalio_bt_transport_unlock();return false;}
    size_t at=0;
    while(at<data.size()&&waited<timeout_ms&&metalio_bt_is_powered()){
        ssize_t n=write(fd_,data.data()+at,data.size()-at);
        if(n>0)at+=n;
        else if(n<0&&errno!=EAGAIN&&errno!=EINTR)break;
        else{usleep(10000);waited+=10;}
    }
    metalio_bt_transport_unlock();return at==data.size();
}

bool SimpleUart::sendModeWait(int mode,std::function<bool()> valid)
{
    if(mode<1||mode>3||!initialized_||fd_<0||(valid&&!valid()))return false;
    std::vector<uint8_t> received;received.reserve(4096);
    unsigned waited=0;
    while(metalio_bt_transport_trylock()<0){
        if(waited>=12000||!metalio_bt_is_powered()||(valid&&!valid()))return false;
        usleep(10000);waited+=10;
    }
    auto active=[&]{return metalio_bt_is_powered()&&(!valid||valid());};
    auto drain=[&]{
        int available=0;
        if(ioctl(fd_,FIONREAD,(unsigned long)&available)<0||available<=0)return;
        uint8_t bytes[256];ssize_t n=read(fd_,bytes,sizeof(bytes));
        if(n>0&&received.size()+(size_t)n<=4096)received.insert(received.end(),bytes,bytes+n);
    };
    auto send=[&](const char*cmd){
        size_t at=0,len=strlen(cmd);unsigned ms=0;
        while(at<len&&active()&&ms<2000){
            ssize_t n=write(fd_,cmd+at,len-at);
            if(n>0)at+=(size_t)n;
            else if(n<0&&errno!=EINTR&&errno!=EAGAIN)break;
            else{usleep(10000);ms+=10;}
        }
        printf("BT_MODE_UART_TX mode=%d complete=%d command=%s",mode,at==len,cmd);
        return at==len;
    };
    bool ok=active()&&send(mode==2?"AT+TX=1\r\n":mode==3?"AT+RX=1\r\n":"AT+RX=2\r\n");
    for(int n=0;ok&&active()&&n<70;n++){usleep(10000);drain();}
    // Do not allow an older queued mode response to satisfy the new command.
    size_t ack_start=received.size();
    char cmd[24],ack[24];snprintf(cmd,sizeof(cmd),"AT+MODE=%d\r\n",mode);snprintf(ack,sizeof(ack),"SET MODE %d",mode);
    ok=ok&&active()&&send(cmd);bool confirmed=false;
    for(int n=0;ok&&active()&&n<400&&!confirmed;n++){
        drain();
        if(received.size()>ack_start){const auto*start=(const char*)received.data()+ack_start;confirmed=std::search(start,(const char*)received.data()+received.size(),ack,ack+strlen(ack))!=(const char*)received.data()+received.size();}
        if(!confirmed)usleep(10000);
    }
    ok=ok&&confirmed&&active();
    metalio_bt_transport_unlock();
    printf("BT_MODE_UART_RESULT mode=%d confirmed=%d bytes=%u\n",mode,ok,(unsigned)received.size());
    // Preserve link/profile URCs received during the transaction. Dispatch
    // only after releasing the transport lock, just like the normal reader.
    if(!received.empty())DispatchReceived(received);
    return ok;
}

void SimpleUart::DispatchReceived(const std::vector<uint8_t>& data)
{
    // Mode transactions and the normal reader must never mutate a callback's
    // line buffer concurrently. Do not hold cb_mutex_ while entering LVGL.
    std::lock_guard<std::mutex> dispatch(init_mutex_);
    std::function<void(const std::vector<uint8_t>&)> callback,observer;
    {std::lock_guard<std::mutex> lock(cb_mutex_);callback=callback_;observer=audio_observer_;}
    if(observer)observer(data);
    if(callback)callback(data);
}

void SimpleUart::registerCallback(
    std::function<void(const std::vector<uint8_t> &)> cb)
{
    std::lock_guard<std::mutex> lock(cb_mutex_);
    callback_ = std::move(cb);
}

void SimpleUart::registerAudioObserver(std::function<void(const std::vector<uint8_t> &)> cb)
{std::lock_guard<std::mutex> lock(cb_mutex_);audio_observer_=std::move(cb);}

void SimpleUart::rxLoop()
{
    uint8_t buf[1024];

    while (initialized_ && fd_ >= 0)
    {
        if (!metalio_bt_is_powered()) {
            usleep(20 * 1000);
            continue;
        }
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int n = poll(&pfd, 1, 20);
        if (n < 0 && errno != EINTR) {usleep(20000);continue;}
        // Some UART lower halves do not notify poll for every FIFO burst.
        // A bounded nonblocking read also drains those bytes without waiting
        // for a second interrupt, while preserving transaction ownership.
        if (metalio_bt_transport_trylock() < 0) {
            usleep(20 * 1000);
            continue;
        }
        // Poll does not consume bytes. Recheck with a nonblocking read after
        // acquiring ownership, because a mode transaction may have read them.
        int available=0;
        ssize_t len = ioctl(fd_,FIONREAD,(unsigned long)&available)==0&&available>0?
            read(fd_,buf,std::min<size_t>(sizeof(buf),(size_t)available)):-1;
        metalio_bt_transport_unlock();
        if (len <= 0)
            continue;

        // Callback may queue LVGL work while a screen unregisters itself.
        // Holding cb_mutex_ here would invert the UI-lock order and hang.
        std::vector<uint8_t> data(buf, buf + len);
        DispatchReceived(data);
    }
}
