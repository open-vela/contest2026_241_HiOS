#include "hi_task_policy.h"
/* SPDX-License-Identifier: Apache-2.0 */
#include "phone_pcm_bridge.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <new>
#include <time.h>
#include <unistd.h>

namespace PhonePcm {
namespace {
constexpr unsigned kIoTimeoutMs = 20;
constexpr unsigned kStartTimeoutMs = 1000;
constexpr unsigned kStallTimeoutMs = 500;
constexpr size_t kMaximumFrame = 160; // 10 ms at 16 kHz
constexpr size_t kMaximumQueue = 1920; // 120 ms at 16 kHz

uint64_t Milliseconds()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000 + unsigned(now.tv_nsec / 1000000);
}

timespec Deadline(unsigned milliseconds)
{
    timespec result{};
    clock_gettime(CLOCK_MONOTONIC, &result);
    result.tv_sec += milliseconds / 1000;
    result.tv_nsec += long(milliseconds % 1000) * 1000000;
    if (result.tv_nsec >= 1000000000) {
        ++result.tv_sec;
        result.tv_nsec -= 1000000000;
    }
    return result;
}

bool Valid(const Port &port)
{
    return port.context && (port.sample_rate == 8000 || port.sample_rate == 16000) &&
        port.open && port.read && port.write && port.cancel && port.close;
}
} // namespace

struct Bridge::Session {
    struct Direction {
        Session *session = nullptr;
        Port source{}, destination{};
        pthread_mutex_t lock{};
        pthread_cond_t changed{};
        bool lock_ready = false, condition_ready = false;
        int16_t queue[kMaximumQueue]{};
        size_t first = 0, used = 0, capacity = 0, target = 0;
        std::atomic<uint32_t> received{0}, written{0};
        std::atomic<bool> transferred{false};
        uint64_t phase = 0;
        int64_t filtered_error = 0;

        int Initialize(Session *owner, const Port &from, const Port &to)
        {
            session = owner;
            source = from;
            destination = to;
            capacity = from.sample_rate * 120 / 1000;
            target = from.sample_rate * 40 / 1000;
            int rc = pthread_mutex_init(&lock, nullptr);
            if (rc) return -rc;
            lock_ready = true;
            pthread_condattr_t attr;
            rc = pthread_condattr_init(&attr);
            if (rc) return -rc;
            // Wall-clock correction/NTP must not extend a PCM wait.
            rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
            if (!rc) rc = pthread_cond_init(&changed, &attr);
            pthread_condattr_destroy(&attr);
            condition_ready = rc == 0;
            return -rc;
        }

        ~Direction()
        {
            if (condition_ready) pthread_cond_destroy(&changed);
            if (lock_ready) pthread_mutex_destroy(&lock);
        }

        void Wake()
        {
            if (!condition_ready) return;
            pthread_mutex_lock(&lock);
            pthread_cond_broadcast(&changed);
            pthread_mutex_unlock(&lock);
        }

        static void *ReadEntry(void *argument)
        {
            static_cast<Direction *>(argument)->ReadLoop();
            return nullptr;
        }

        static void *WriteEntry(void *argument)
        {
            static_cast<Direction *>(argument)->WriteLoop();
            return nullptr;
        }

        void ReadLoop()
        {
            int16_t frame[kMaximumFrame];
            const size_t count = source.sample_rate / 100;
            uint64_t last_data = Milliseconds();
            while (!session->stop.load()) {
                int got = source.read(source.context, frame, count, kIoTimeoutMs);
                if (session->stop.load()) break;
                if (got == -EAGAIN || got == -ETIMEDOUT || got == -EINTR) got = 0;
                if (got < 0 || size_t(got) > count) {
                    session->Fail(got < 0 ? got : -EPROTO);
                    break;
                }
                if (!got) {
                    if (Milliseconds() - last_data >= kStallTimeoutMs)
                        session->Fail(-ETIMEDOUT);
                    else usleep(1000); // Also bound a backend that returns early.
                    continue;
                }
                last_data = Milliseconds();
                pthread_mutex_lock(&lock);
                bool overflow = size_t(got) > capacity - used;
                if (!overflow) {
                    for (int i = 0; i < got; ++i)
                        queue[(first + used + size_t(i)) % capacity] = frame[i];
                    used += size_t(got);
                    received.fetch_add(uint32_t(got));
                    pthread_cond_signal(&changed);
                }
                pthread_mutex_unlock(&lock);
                if (overflow) session->Fail(-EOVERFLOW);
            }
        }

        void WriteLoop()
        {
            int16_t output[kMaximumFrame];
            const size_t count = destination.sample_rate / 100;
            const uint64_t nominal = (uint64_t(source.sample_rate) << 32) /
                destination.sample_rate;
            bool primed = false;
            uint64_t last_frame = Milliseconds();
            while (!session->stop.load()) {
                pthread_mutex_lock(&lock);
                // Slowly match independent sample clocks; do not delete or
                // repeat whole voice frames to correct normal clock drift.
                int64_t error = int64_t(used) - int64_t(target);
                filtered_error = (filtered_error * 63 + error * 1024) / 64;
                int ppm = int(filtered_error * 1000000 /
                    (int64_t(source.sample_rate) * 1024));
                ppm = std::max(-1000, std::min(1000, ppm));
                uint64_t step = nominal * uint64_t(1000000 + ppm) / 1000000;
                size_t needed = size_t((phase + step * (count - 1)) >> 32) + 2;
                needed = std::max(needed, size_t((phase + step * count) >> 32));
                size_t required = primed ? needed : std::max(target, needed);
                if (used < required && !session->stop.load()) {
                    timespec until = Deadline(kIoTimeoutMs);
                    int rc = pthread_cond_timedwait(&changed, &lock, &until);
                    pthread_mutex_unlock(&lock);
                    if (rc && rc != ETIMEDOUT && rc != EINTR) session->Fail(-rc);
                    if (Milliseconds() - last_frame >= kStallTimeoutMs)
                        session->Fail(-ETIMEDOUT);
                    continue;
                }
                if (session->stop.load()) {
                    pthread_mutex_unlock(&lock);
                    break;
                }
                primed = true;
                uint64_t position = phase;
                for (size_t i = 0; i < count; ++i, position += step) {
                    size_t index = size_t(position >> 32);
                    int32_t a = queue[(first + index) % capacity];
                    int32_t b = queue[(first + index + 1) % capacity];
                    output[i] = int16_t(a + int64_t(b - a) *
                        uint32_t(position) / int64_t(1ULL << 32));
                }
                size_t consumed = size_t(position >> 32);
                first = (first + consumed) % capacity;
                used -= consumed;
                phase = uint32_t(position);
                pthread_mutex_unlock(&lock);

                size_t sent = 0;
                const uint64_t deadline = Milliseconds() + 100;
                while (sent < count && !session->stop.load()) {
                    int n = destination.write(destination.context, output + sent,
                        count - sent, kIoTimeoutMs);
                    if (session->stop.load()) break;
                    if (n == -EAGAIN || n == -ETIMEDOUT || n == -EINTR) n = 0;
                    if (n < 0 || size_t(n) > count - sent) {
                        session->Fail(n < 0 ? n : -EPROTO);
                        break;
                    }
                    sent += size_t(n);
                    if (Milliseconds() >= deadline && sent < count)
                        session->Fail(-ETIMEDOUT);
                    if (!n) usleep(1000);
                }
                written.fetch_add(uint32_t(sent));
                if (sent == count) {
                    transferred.store(true);
                    last_frame = Milliseconds();
                }
            }
        }
    } downlink, uplink;

    Port modem{}, headset{};
    bool modem_open = false, headset_open = false;
    pthread_t threads[4]{};
    size_t created = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> error{0};

    void Fail(int reason)
    {
        int expected = 0;
        error.compare_exchange_strong(expected, reason < 0 ? reason : -EIO);
        stop.store(true);
        downlink.Wake();
        uplink.Wake();
    }

    Statistics Snapshot() const
    {
        return {downlink.received.load(), downlink.written.load(),
            uplink.received.load(), uplink.written.load()};
    }
};

Bridge::Bridge() = default;
Bridge::~Bridge() { Stop(); pthread_mutex_destroy(&control_); }

int Bridge::Start(const Port &modem, const Port &headset)
{
    pthread_mutex_lock(&control_);
    if (session_) {
        pthread_mutex_unlock(&control_);
        return -EBUSY;
    }
    last_error_ = 0;
    last_statistics_ = {};
    if (!Valid(modem) || !Valid(headset) || modem.context == headset.context) {
        last_error_ = -EINVAL;
        pthread_mutex_unlock(&control_);
        return last_error_;
    }
    session_ = new (std::nothrow) Session;
    if (!session_) {
        last_error_ = -ENOMEM;
        pthread_mutex_unlock(&control_);
        return -ENOMEM;
    }
    Session *s = session_;
    s->modem = modem;
    s->headset = headset;
    int rc = s->downlink.Initialize(s, modem, headset);
    if (!rc) rc = s->uplink.Initialize(s, headset, modem);
    if (!rc) { rc = modem.open(modem.context); s->modem_open = rc == 0; }
    if (!rc) { rc = headset.open(headset.context); s->headset_open = rc == 0; }
    if (rc > 0) rc = -EPROTO;
    pthread_attr_t attr;
    bool attr_ready = false;
    if (!rc) {
        rc = pthread_attr_init(&attr);
        attr_ready = rc == 0;
        if (rc) rc = -rc;
    }
    if (!rc) { int n = pthread_attr_setstacksize(&attr, 16384); if (n) rc = -n; }
    for (size_t i = 0; !rc && i < 4; ++i) {
        Session::Direction *d = i < 2 ? &s->downlink : &s->uplink;
        int n = hi_task_create_legacy(&s->threads[i], &attr,
            i % 2 ? Session::Direction::WriteEntry : Session::Direction::ReadEntry, d);
        if (n) rc = -n;
        else ++s->created;
    }
    if (attr_ready) pthread_attr_destroy(&attr);
    const uint64_t deadline = Milliseconds() + kStartTimeoutMs;
    while (!rc && !s->error.load() &&
           (!s->downlink.transferred.load() || !s->uplink.transferred.load())) {
        if (Milliseconds() >= deadline) { rc = -ETIMEDOUT; break; }
        usleep(5000);
    }
    if (!rc) rc = s->error.load();
    if (rc) {
        s->Fail(rc);
        StopLocked();
    }
    pthread_mutex_unlock(&control_);
    return rc;
}

int Bridge::StopLocked()
{
    if (!session_) return 0;
    Session *s = session_;
    for (size_t i = 0; i < s->created; ++i)
        if (pthread_equal(pthread_self(), s->threads[i])) return -EDEADLK;
    s->stop.store(true);
    s->downlink.Wake();
    s->uplink.Wake();
    if (s->headset_open) s->headset.cancel(s->headset.context);
    if (s->modem_open) s->modem.cancel(s->modem.context);
    for (size_t i = 0; i < s->created; ++i) pthread_join(s->threads[i], nullptr);
    last_error_ = s->error.load();
    last_statistics_ = s->Snapshot();
    if (s->headset_open) s->headset.close(s->headset.context);
    if (s->modem_open) s->modem.close(s->modem.context);
    delete s;
    session_ = nullptr;
    return 0;
}

int Bridge::Stop()
{
    pthread_mutex_lock(&control_);
    int rc = StopLocked();
    pthread_mutex_unlock(&control_);
    return rc;
}

State Bridge::GetState()
{
    pthread_mutex_lock(&control_);
    State result = !session_ ? State::Idle : session_->error.load() ? State::Fault :
        (session_->downlink.transferred.load() && session_->uplink.transferred.load()) ?
        State::Running : State::Starting;
    pthread_mutex_unlock(&control_);
    return result;
}

int Bridge::LastError()
{
    pthread_mutex_lock(&control_);
    int result = session_ ? session_->error.load() : last_error_;
    pthread_mutex_unlock(&control_);
    return result;
}

Statistics Bridge::GetStatistics()
{
    pthread_mutex_lock(&control_);
    Statistics result = session_ ? session_->Snapshot() : last_statistics_;
    pthread_mutex_unlock(&control_);
    return result;
}
} // namespace PhonePcm
