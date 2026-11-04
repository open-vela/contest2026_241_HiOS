#include "hi_task_policy.h"
#include "ha_qr_scanner.h"
#include "quirc/quirc.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <unistd.h>

namespace {
constexpr int kWidth = 720, kHeight = 600;
std::mutex s_frames, s_results;
std::atomic<bool> s_active{false};
std::atomic<bool> s_has_frame{false};
std::atomic<unsigned> s_generation{0};
uint8_t *s_gray = nullptr;
bool s_ready = false, s_worker_started = false;
HaQrResult s_result = kHaQrNone;
std::string s_token;

void Result(unsigned generation, HaQrResult result, const std::string &token = "")
{
    std::lock_guard<std::mutex> guard(s_results);
    if (s_active.load() && generation == s_generation.load()) {
        s_result = result;
        s_token = token;
    }
}

void *Worker(void *)
{
    quirc *decoder = quirc_new();
    const bool initialized = decoder && quirc_resize(decoder, kWidth, kHeight) == 0;
    for (;;) {
        if (!s_active.load()) { usleep(100000); continue; }
        const unsigned generation = s_generation.load();
        if (!initialized) { Result(generation, kHaQrFailure); usleep(200000); continue; }
        bool frame = false;
        {
            std::lock_guard<std::mutex> guard(s_frames);
            if (s_ready) {
                std::memcpy(quirc_begin(decoder, nullptr, nullptr), s_gray, kWidth * kHeight);
                s_ready = false;
                frame = true;
            }
        }
        if (!frame) { usleep(100000); continue; }
        quirc_end(decoder);
        for (int i = 0; i < quirc_count(decoder) && i < 4; ++i) {
            quirc_code code;
            quirc_data data;
            quirc_extract(decoder, i, &code);
            auto error = quirc_decode(&code, &data);
            if (error == QUIRC_ERROR_DATA_ECC) {
                quirc_flip(&code);
                error = quirc_decode(&code, &data);
            }
            if (error != QUIRC_SUCCESS) continue;
            std::string token;
            if (HaQrToken_Parse(data.payload, data.payload_len, &token)) {
                Result(generation, kHaQrToken, token);
                break;
            }
            Result(generation, kHaQrNotToken);
        }
        usleep(250000);
    }
    return nullptr;
}
}

bool HaQrScanner_Start()
{
    HaQrScanner_Stop();
    s_has_frame.store(false);
    if (!s_gray) s_gray = static_cast<uint8_t *>(std::malloc(kWidth * kHeight));
    if (!s_gray) return false;
    if (!s_worker_started) {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 32768);
        s_worker_started = hi_task_create_legacy(&thread, &attr, Worker, nullptr) == 0;
        pthread_attr_destroy(&attr);
        if (!s_worker_started) return false;
    }
    {
        std::lock_guard<std::mutex> guard(s_frames);
        s_ready = false;
    }
    s_active.store(true);
    return true;
}

void HaQrScanner_Stop()
{
    s_active.store(false);
    ++s_generation;
    std::lock_guard<std::mutex> guard(s_results);
    s_result = kHaQrNone;
    s_token.clear();
}

void HaQrScanner_Frame(const uint8_t *bgr, int width, int height)
{
    if (!s_active.load() || !bgr || width != kWidth || height != kHeight) return;
    // Never wait on the camera/LVGL thread when the decoder copies a frame.
    std::unique_lock<std::mutex> guard(s_frames, std::try_to_lock);
    if (!guard.owns_lock() || s_ready || !s_active.load()) return;
    for (size_t i = 0; i < kWidth * kHeight; ++i) {
        const uint8_t *p = bgr + i * 3;
        s_gray[i] = (29 * p[0] + 150 * p[1] + 77 * p[2]) >> 8;
    }
    s_ready = true;
    s_has_frame.store(true);
}

bool HaQrScanner_HasFrame() { return s_has_frame.load(); }

HaQrResult HaQrScanner_Poll(std::string *token)
{
    std::lock_guard<std::mutex> guard(s_results);
    const auto result = s_result;
    if (token && result == kHaQrToken) *token = s_token;
    s_token.clear();
    s_result = kHaQrNone;
    return result;
}
