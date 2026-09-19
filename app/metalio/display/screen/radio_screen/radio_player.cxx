/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * RadioPlayer — rewritten for stable playback on ESP32-P4 / NuttX.
 *
 * Design goals vs the previous state machine:
 *   - One session thread owns HLS create/run/destroy
 *   - One PCM drain thread writes fixed 960-sample chunks every ~55 ms
 *     (same pacing as TTS — avoids starving display GDMA / blue screen)
 *   - No dedicated spectrum thread; bands updated from the drain path
 *   - Flat flags (want_play / leave) instead of Starting/Stopping/zombie
 *
 * Pipeline: progressive HTTP MP3 → esp_audio_simple_player + HTTP IO
 *           → OutCallback → 16 kHz mono ring → PcmDrain → /dev/audio/pcm0
 * Soft-start: open I2S only after first PCM; fade-in; fail-once on error.
 */

#include "radio_player.h"

#include "application.h"
#include "audio_service.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_log_shim.h"
#include "media_lib_adapter.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include <nuttx/audio/audio.h>

namespace {

constexpr size_t kPlayStack = 48 * 1024;
constexpr size_t kDrainStack = 16 * 1024;
constexpr size_t kPcmRingSize = 4096;
constexpr size_t kI2sChunkSamples = 960;
constexpr int kDecodeBlockFrames = 256;
constexpr int kBarCount = RadioPlayer::kBandCount;
constexpr int kRadioSampleRate = RadioPlayer::kSampleRate;

std::atomic<int> s_station_index{kDefaultRadioStationIndex};
std::atomic<int> s_status{static_cast<int>(RadioStatus::Idle)};
std::atomic<bool> s_want_play{false};
std::atomic<bool> s_leave{false};
std::atomic<bool> s_session_alive{false};
std::atomic<bool> s_reported_playing{false};
std::atomic<bool> s_media_lib_ready{false};
std::atomic<bool> s_audio_occupied{false};
std::atomic<int> s_pcm_channels{2};
std::atomic<int> s_pcm_sample_rate{44100};
std::atomic<int> s_volume{70};
std::atomic<bool> s_asp_stop_busy{false};
/* Soft-start: ramp PCM gain after OpenTx to avoid DAC crack → blue. */
std::atomic<int> s_fade_left{0};
std::atomic<bool> s_tx_armed{false};

int s_tx_fd = -1;
std::mutex s_tx_mu;
std::vector<int32_t> s_tx_scratch;
std::atomic<bool> s_tx_logged{false};

std::mutex s_bar_mu;
uint8_t s_bar_levels[kBarCount] = {};

pthread_t s_drain_thread{};
bool s_drain_joinable = false;
std::atomic<bool> s_drain_run{false};

esp_asp_handle_t s_player = nullptr;

struct PcmRing {
    int16_t buf[kPcmRingSize];
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
};
PcmRing s_pcm = {};
std::mutex s_pcm_mu;

OpusResampler s_resampler;
std::atomic<int> s_resampler_rate{0};
std::vector<int16_t> s_mono_buf;
std::vector<int16_t> s_resampled_buf;

void SetStatus(RadioStatus kind)
{
    s_status.store(static_cast<int>(kind), std::memory_order_relaxed);
}

int ClampStationIndex(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(kRadioStationCount)) {
        return kDefaultRadioStationIndex;
    }
    return idx;
}

const RadioStation &StationAt(int idx)
{
    return kRadioStations[static_cast<size_t>(ClampStationIndex(idx))];
}

bool SpawnThread(pthread_t *out, void *(*entry)(void *), void *arg,
                 size_t stack_bytes)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    pthread_t th = 0;
    const int rc = pthread_create(&th, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        return false;
    }
    if (out != nullptr) {
        *out = th;
    } else {
        pthread_detach(th);
    }
    return true;
}

void JoinThread(pthread_t *th, bool *joinable)
{
    if (th == nullptr || joinable == nullptr || !*joinable) {
        return;
    }
    pthread_join(*th, nullptr);
    *joinable = false;
    *th = {};
}

void ClearBars()
{
    std::lock_guard<std::mutex> lock(s_bar_mu);
    std::memset(s_bar_levels, 0, sizeof(s_bar_levels));
}

void UpdateBandsFromPcm(const int16_t *samples, size_t n)
{
    if (samples == nullptr || n == 0) {
        return;
    }
    /* Time-domain energy bands: split the chunk into 12 slices. Cheap,
     * no extra thread, enough motion for the visualizer. */
    uint8_t levels[kBarCount];
    const size_t slice = std::max<size_t>(1, n / static_cast<size_t>(kBarCount));
    for (int b = 0; b < kBarCount; ++b) {
        const size_t begin = static_cast<size_t>(b) * slice;
        if (begin >= n) {
            levels[b] = 0;
            continue;
        }
        const size_t end = std::min(n, begin + slice);
        uint64_t acc = 0;
        for (size_t i = begin; i < end; ++i) {
            const int v = samples[i];
            acc += static_cast<uint64_t>(v < 0 ? -v : v);
        }
        const size_t count = end - begin;
        const uint32_t mean =
            count > 0 ? static_cast<uint32_t>(acc / count) : 0;
        /* Map ~0..8000 toward 0..255 with mild boost on lower bars. */
        uint32_t scaled = (mean * 255u) / 6000u;
        if (b < 4) {
            scaled = (scaled * 5u) / 4u;
        }
        if (scaled > 255u) {
            scaled = 255u;
        }
        levels[b] = static_cast<uint8_t>(scaled);
    }

    std::lock_guard<std::mutex> lock(s_bar_mu);
    for (int i = 0; i < kBarCount; ++i) {
        const int prev = s_bar_levels[i];
        const int next = levels[i];
        /* Attack fast / release slow so bars stay readable at 55 ms cadence. */
        s_bar_levels[i] = static_cast<uint8_t>(
            next >= prev ? prev + (next - prev + 1) / 2
                         : prev - (prev - next + 3) / 4);
    }
}

void PushPcm(const int16_t *samples, size_t n)
{
    if (samples == nullptr || n == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_pcm_mu);
    for (size_t i = 0; i < n; ++i) {
        s_pcm.buf[s_pcm.head] = samples[i];
        s_pcm.head = (s_pcm.head + 1) % kPcmRingSize;
        if (s_pcm.count < kPcmRingSize) {
            s_pcm.count++;
        } else {
            s_pcm.tail = (s_pcm.tail + 1) % kPcmRingSize;
        }
    }
}

size_t PopPcm(int16_t *out, size_t n)
{
    std::lock_guard<std::mutex> lock(s_pcm_mu);
    const size_t to_read = (s_pcm.count < n) ? s_pcm.count : n;
    for (size_t i = 0; i < to_read; ++i) {
        out[i] = s_pcm.buf[s_pcm.tail];
        s_pcm.tail = (s_pcm.tail + 1) % kPcmRingSize;
    }
    s_pcm.count -= to_read;
    return to_read;
}

void ClearPcm()
{
    std::lock_guard<std::mutex> lock(s_pcm_mu);
    s_pcm.head = s_pcm.tail = s_pcm.count = 0;
}

int OutCallback(uint8_t *data, int data_size, void * /*ctx*/)
{
    if (!s_want_play.load(std::memory_order_relaxed) ||
        s_leave.load(std::memory_order_relaxed)) {
        return 0;
    }
    if (data == nullptr || data_size <= 0) {
        return 0;
    }

    const int16_t *pcm = reinterpret_cast<const int16_t *>(data);
    const int total_samples = data_size / static_cast<int>(sizeof(int16_t));
    const int channels = s_pcm_channels.load(std::memory_order_relaxed);
    if (channels <= 0 || total_samples <= 0) {
        return 0;
    }
    const int frames = total_samples / channels;
    if (frames <= 0) {
        return 0;
    }

    const int src_rate = s_pcm_sample_rate.load(std::memory_order_relaxed);
    int frame_off = 0;
    while (frame_off < frames) {
        if (!s_want_play.load(std::memory_order_relaxed) ||
            s_leave.load(std::memory_order_relaxed)) {
            return 0;
        }
        const int block = std::min(frames - frame_off, kDecodeBlockFrames);
        s_mono_buf.resize(static_cast<size_t>(block));
        for (int i = 0; i < block; ++i) {
            int32_t mix = 0;
            const int src = (frame_off + i) * channels;
            for (int c = 0; c < channels; ++c) {
                mix += pcm[src + c];
            }
            s_mono_buf[static_cast<size_t>(i)] =
                static_cast<int16_t>(mix / channels);
        }

        const int16_t *out = s_mono_buf.data();
        size_t out_n = s_mono_buf.size();
        if (src_rate > 0 && src_rate != kRadioSampleRate) {
            if (s_resampler_rate.load(std::memory_order_relaxed) != src_rate) {
                s_resampler.Configure(src_rate, kRadioSampleRate);
                s_resampler_rate.store(src_rate, std::memory_order_relaxed);
            }
            const size_t resampled_n =
                s_resampler.GetOutputSamples(s_mono_buf.size());
            s_resampled_buf.resize(resampled_n);
            s_resampler.Process(s_mono_buf.data(), s_mono_buf.size(),
                                s_resampled_buf.data());
            out = s_resampled_buf.data();
            out_n = s_resampled_buf.size();
        }

        PushPcm(out, out_n);
        frame_off += block;
    }

    if (!s_reported_playing.exchange(true, std::memory_order_acq_rel)) {
        write(1, "RADIO_PCM\n", 10);
        SetStatus(RadioStatus::Playing);
    }
    return 0;
}

int EventCallback(esp_asp_event_pkt_t *pkt, void * /*ctx*/)
{
    if (pkt == nullptr) {
        return 0;
    }
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE && pkt->payload != nullptr &&
        pkt->payload_size >= static_cast<int>(sizeof(esp_asp_state_t))) {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        std::memcpy(&state, pkt->payload, sizeof(state));
        if (state == ESP_ASP_STATE_RUNNING &&
            s_want_play.load(std::memory_order_relaxed)) {
            /* Stay Connecting until first OutCallback PCM — avoids false Playing. */
        } else if (state == ESP_ASP_STATE_ERROR) {
            SetStatus(RadioStatus::Failed);
            s_want_play.store(false, std::memory_order_relaxed);
        }
    } else if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
               pkt->payload != nullptr &&
               pkt->payload_size >=
                   static_cast<int>(sizeof(esp_asp_music_info_t))) {
        esp_asp_music_info_t info = {};
        std::memcpy(&info, pkt->payload, sizeof(info));
        if (info.channels > 0) {
            s_pcm_channels.store(info.channels, std::memory_order_relaxed);
        }
        if (info.sample_rate > 0) {
            s_pcm_sample_rate.store(info.sample_rate,
                                    std::memory_order_relaxed);
        }
        write(1, "RADIO_INFO\n", 11);
    }
    return 0;
}

void EnsureMediaLibAdapter()
{
    if (s_media_lib_ready.load(std::memory_order_relaxed)) {
        return;
    }
    media_lib_add_default_adapter();
    s_media_lib_ready.store(true, std::memory_order_relaxed);
}

esp_gmf_err_t HttpTlsGetScore(esp_gmf_io_handle_t /*handle*/, const char *url,
                              int *score)
{
    *score = ESP_GMF_IO_SCORE_NONE;
    if (url == nullptr) {
        return ESP_GMF_ERR_OK;
    }
    /* Prefer HTTP IO for progressive audio; leave .m3u8 to HLS IO. */
    if (strstr(url, ".m3u8") != nullptr || strstr(url, ".M3U8") != nullptr) {
        return ESP_GMF_ERR_OK;
    }
    if (strncasecmp(url, "http://", 7) == 0 ||
        strncasecmp(url, "https://", 8) == 0) {
        *score = ESP_GMF_IO_SCORE_STANDARD + 10;
    }
    return ESP_GMF_ERR_OK;
}

bool RegisterHttp(esp_asp_handle_t player)
{
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.crt_bundle_attach = nullptr;
    http_cfg.io_cfg.thread.stack = 48 * 1024;

    esp_gmf_io_handle_t http_io = nullptr;
    if (esp_gmf_io_http_init(&http_cfg, &http_io) != ESP_GMF_ERR_OK ||
        http_io == nullptr) {
        write(1, "RADIO_HTTP_FAIL\n", 16);
        return false;
    }
    reinterpret_cast<esp_gmf_io_t *>(http_io)->get_score = HttpTlsGetScore;
    if (esp_audio_simple_player_register_io(player, http_io) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(http_io);
        return false;
    }
    return true;
}

void DestroyPlayer()
{
    if (s_player == nullptr) {
        return;
    }
    esp_asp_handle_t player = s_player;
    s_player = nullptr;
    esp_audio_simple_player_stop(player);
    esp_audio_simple_player_destroy(player);
}

bool CreatePlayer()
{
    DestroyPlayer();
    s_pcm_channels.store(2, std::memory_order_relaxed);
    s_pcm_sample_rate.store(44100, std::memory_order_relaxed);
    s_resampler_rate.store(0, std::memory_order_relaxed);

    esp_asp_cfg_t cfg = {};
    cfg.out.cb = OutCallback;
    cfg.out.user_ctx = nullptr;
    cfg.task_prio = 5;
    cfg.task_stack = 32 * 1024;
    cfg.prev = nullptr;
    cfg.prev_ctx = nullptr;

    if (esp_audio_simple_player_new(&cfg, &s_player) != ESP_GMF_ERR_OK ||
        s_player == nullptr) {
        write(1, "RADIO_NEW_FAIL\n", 15);
        return false;
    }

    /* Progressive MP3 only — skip HLS (48k stack + stuck Connecting). */
    if (!RegisterHttp(s_player)) {
        DestroyPlayer();
        return false;
    }

    esp_audio_simple_player_set_event(s_player, EventCallback, nullptr);
    return true;
}

void AspStopWorker()
{
    esp_asp_handle_t player = s_player;
    if (player != nullptr) {
        esp_audio_simple_player_stop(player);
    }
    s_asp_stop_busy.store(false, std::memory_order_release);
}

void *AspStopThreadEntry(void * /*arg*/)
{
    AspStopWorker();
    return nullptr;
}

void RequestAspStop()
{
    bool expected = false;
    if (!s_asp_stop_busy.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!SpawnThread(nullptr, AspStopThreadEntry, nullptr, 8 * 1024)) {
        s_asp_stop_busy.store(false, std::memory_order_release);
        AspStopWorker();
    }
}

void OpenTx()
{
    std::lock_guard<std::mutex> lock(s_tx_mu);
    if (s_tx_fd >= 0) {
        return;
    }
    s_tx_fd = open("/dev/audio/pcm0", O_WRONLY | O_NONBLOCK);
    if (s_tx_fd < 0) {
        s_tx_fd = open("/dev/audio/pcm0", O_WRONLY);
    }
    if (s_tx_fd < 0) {
        return;
    }
    struct audio_caps_s caps;
    std::memset(&caps, 0, sizeof(caps));
    caps.ac_type = AUDIO_TYPE_OUTPUT;
    caps.ac_subtype = AUDIO_FMT_PCM;
    caps.ac_channels = 2;
    caps.ac_controls.hw[0] = 16000 & 0xffff;
    caps.ac_controls.b[3] = (16000 >> 16) & 0xff;
    caps.ac_controls.b[2] = 32;
    ioctl(s_tx_fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&caps);
    ioctl(s_tx_fd, AUDIOIOC_START, 0);
    s_tx_logged.store(false, std::memory_order_relaxed);
    /* ~240 ms gain ramp after open — avoids DAC crack → blue. */
    s_fade_left.store(kRadioSampleRate * 24 / 100, std::memory_order_relaxed);
}

void CloseTx()
{
    std::lock_guard<std::mutex> lock(s_tx_mu);
    if (s_tx_fd >= 0) {
        ioctl(s_tx_fd, AUDIOIOC_STOP, 0);
        close(s_tx_fd);
        s_tx_fd = -1;
    }
    s_tx_logged.store(false, std::memory_order_relaxed);
    s_tx_armed.store(false, std::memory_order_relaxed);
    s_fade_left.store(0, std::memory_order_relaxed);
}

void WriteI2sChunk(const int16_t *data, size_t samples)
{
    if (data == nullptr || samples == 0) {
        return;
    }

    int vol = s_volume.load(std::memory_order_relaxed);
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    const double v = static_cast<double>(vol) / 100.0;
    const double v2 = v * v;
    const int32_t vf_full = static_cast<int32_t>(v2 * 65536.0 + 0.5);
    int fade = s_fade_left.load(std::memory_order_relaxed);
    const int fade_total = kRadioSampleRate * 24 / 100;

    const size_t need = samples * 2;
    if (s_tx_scratch.size() < need) {
        s_tx_scratch.resize(need);
    }
    for (size_t i = 0; i < samples; ++i) {
        int32_t vf = vf_full;
        if (fade > 0 && fade_total > 0) {
            const double f =
                1.0 - static_cast<double>(fade) / static_cast<double>(fade_total);
            const double g = (f < 0.0) ? 0.0 : (f > 1.0 ? 1.0 : f);
            vf = static_cast<int32_t>(v2 * g * g * 65536.0 + 0.5);
            --fade;
        }
        const int64_t t =
            static_cast<int64_t>(data[i]) * static_cast<int64_t>(vf);
        int32_t p;
        if (t > INT32_MAX) {
            p = INT32_MAX;
        } else if (t < INT32_MIN) {
            p = INT32_MIN;
        } else {
            p = static_cast<int32_t>(t);
        }
        s_tx_scratch[i * 2] = p;
        s_tx_scratch[i * 2 + 1] = p;
    }
    s_fade_left.store(fade > 0 ? fade : 0, std::memory_order_relaxed);

    const size_t total = need * sizeof(int32_t);
    size_t off = 0;
    std::lock_guard<std::mutex> lock(s_tx_mu);
    if (s_tx_fd < 0) {
        return;
    }
    while (off < total) {
        const ssize_t w =
            write(s_tx_fd,
                  reinterpret_cast<const uint8_t *>(s_tx_scratch.data()) + off,
                  total - off);
        if (w <= 0) {
            return;
        }
        off += static_cast<size_t>(w);
    }
    if (!s_tx_logged.exchange(true, std::memory_order_acq_rel)) {
        write(1, "RADIO_I2S\n", 10);
    }
}

void *PcmDrainEntry(void * /*arg*/)
{
    int16_t chunk[kI2sChunkSamples];
    while (s_drain_run.load(std::memory_order_relaxed)) {
        if (!s_want_play.load(std::memory_order_relaxed) ||
            s_leave.load(std::memory_order_relaxed)) {
            usleep(10000);
            continue;
        }

        const size_t got = PopPcm(chunk, kI2sChunkSamples);
        if (got == 0) {
            usleep(8000);
            continue;
        }

        /* Open DAC only after real PCM — empty OpenTx cracked then blue'd. */
        if (!s_tx_armed.exchange(true, std::memory_order_acq_rel)) {
            OpenTx();
            write(1, "RADIO_TX\n", 9);
        }

        UpdateBandsFromPcm(chunk, got);

        size_t off = 0;
        while (off < got) {
            if (!s_want_play.load(std::memory_order_relaxed) ||
                s_leave.load(std::memory_order_relaxed)) {
                break;
            }
            const size_t n = std::min(got - off, kI2sChunkSamples);
            WriteI2sChunk(chunk + off, n);
            off += n;
            usleep(60000);
        }
    }
    CloseTx();
    return nullptr;
}

void StartDrain()
{
    s_drain_run.store(false, std::memory_order_release);
    JoinThread(&s_drain_thread, &s_drain_joinable);
    ClearPcm();
    s_drain_run.store(true, std::memory_order_release);
    if (!SpawnThread(&s_drain_thread, PcmDrainEntry, nullptr, kDrainStack)) {
        s_drain_run.store(false, std::memory_order_relaxed);
        write(1, "RADIO_OUT_FAIL\n", 15);
        return;
    }
    s_drain_joinable = true;
    write(1, "RADIO_OUT\n", 10);
}

void StopDrain()
{
    s_drain_run.store(false, std::memory_order_release);
    JoinThread(&s_drain_thread, &s_drain_joinable);
    ClearPcm();
    ClearBars();
}

void OccupySystemAudio()
{
    if (s_audio_occupied.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    write(1, "RADIO_OCC0\n", 11);
    auto &as = Application::GetInstance().GetAudioService();
    /* Soft occupy only — EnableVoiceProcessing(false) on this worker path
     * cracked the DAC then solid-blued the display flush. */
    as.ArmDownlinkPlaybackProtect();
    int vol = as.GetVolume();
    if (vol < 10) {
        vol = 70;
    }
    if (vol > 100) {
        vol = 100;
    }
    s_volume.store(vol, std::memory_order_relaxed);
    write(1, "RADIO_OCC1\n", 11);
}

void ReleaseSystemAudio()
{
    if (!s_audio_occupied.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    auto &as = Application::GetInstance().GetAudioService();
    as.ClearDownlinkPlaybackProtect();
    /* Do not re-enable wake word here — chat/home owns that lifecycle. */
}

void SessionMain()
{
    write(1, "RADIO_PLAY\n", 11);
    OccupySystemAudio();
    EnsureMediaLibAdapter();
    write(1, "RADIO_ADAPT\n", 12);

    ClearPcm();
    ClearBars();
    s_reported_playing.store(false, std::memory_order_relaxed);
    s_tx_armed.store(false, std::memory_order_relaxed);

    {
        auto &as = Application::GetInstance().GetAudioService();
        as.SetTtsUplinkGate(false);
        /* Do not ResetDecoder here — hard reset + OpenTx cracked then blue'd. */
    }

    StartDrain();

    if (!CreatePlayer()) {
        write(1, "RADIO_NEW_FAIL\n", 15);
        SetStatus(RadioStatus::Failed);
        StopDrain();
        ReleaseSystemAudio();
        s_session_alive.store(false, std::memory_order_release);
        return;
    }
    write(1, "RADIO_ASP_OK\n", 13);

    while (!s_leave.load(std::memory_order_relaxed)) {
        if (!s_want_play.load(std::memory_order_relaxed)) {
            usleep(40000);
            continue;
        }

        /* Ensure previous run_to_end fully stopped before restarting. */
        for (int i = 0; i < 50 && s_asp_stop_busy.load(std::memory_order_relaxed);
             ++i) {
            usleep(20000);
        }

        if (s_player != nullptr) {
            esp_asp_state_t asp_st = ESP_ASP_STATE_NONE;
            if (esp_audio_simple_player_get_state(s_player, &asp_st) ==
                    ESP_GMF_ERR_OK &&
                (asp_st == ESP_ASP_STATE_RUNNING ||
                 asp_st == ESP_ASP_STATE_PAUSED)) {
                esp_audio_simple_player_stop(s_player);
                usleep(50000);
            }
        }

        if (s_leave.load(std::memory_order_relaxed)) {
            break;
        }
        if (!s_want_play.load(std::memory_order_relaxed)) {
            continue;
        }

        s_reported_playing.store(false, std::memory_order_relaxed);
        ClearPcm();
        ClearBars();
        SetStatus(RadioStatus::Connecting);

        const char *url =
            StationAt(s_station_index.load(std::memory_order_relaxed)).url;
        write(1, "RADIO_RUN\n", 10);

        const esp_gmf_err_t err =
            esp_audio_simple_player_run_to_end(s_player, url, nullptr);

        if (s_leave.load(std::memory_order_relaxed)) {
            break;
        }
        if (!s_want_play.load(std::memory_order_relaxed)) {
            continue;
        }
        if (err != ESP_GMF_ERR_OK) {
            write(1, "RADIO_RUN_FAIL\n", 15);
            /* Fail once — no 1.2s reconnect storm under audio occupy. */
            SetStatus(RadioStatus::Failed);
            s_want_play.store(false, std::memory_order_relaxed);
            continue;
        }
        write(1, "RADIO_RUN_OK\n", 13);
        usleep(100000);
    }

    write(1, "RADIO_PLAY_END\n", 15);
    DestroyPlayer();
    StopDrain();
    s_mono_buf.clear();
    s_resampled_buf.clear();
    s_tx_scratch.clear();

    ReleaseSystemAudio();
    SetStatus(RadioStatus::Idle);
    s_session_alive.store(false, std::memory_order_release);
    write(1, "RADIO_STOP_DONE\n", 16);

    /* Late Start() while we were tearing down — spawn a fresh session. */
    if (s_want_play.load(std::memory_order_relaxed) &&
        !s_leave.load(std::memory_order_relaxed)) {
        bool expected = false;
        if (s_session_alive.compare_exchange_strong(expected, true)) {
            OccupySystemAudio();
            if (!SpawnThread(nullptr,
                             [](void *) -> void * {
                                 SessionMain();
                                 return nullptr;
                             },
                             nullptr, kPlayStack)) {
                s_session_alive.store(false, std::memory_order_release);
                ReleaseSystemAudio();
                SetStatus(RadioStatus::Failed);
            }
        }
    }
}

void *SessionThreadEntry(void * /*arg*/)
{
    SessionMain();
    return nullptr;
}

void EnsureSession()
{
    bool expected = false;
    if (!s_session_alive.compare_exchange_strong(expected, true)) {
        return;
    }
    write(1, "RADIO_SPWN\n", 11);
    /* Detached: Stop() must not join from LVGL. Session self-exits. */
    if (!SpawnThread(nullptr, SessionThreadEntry, nullptr, kPlayStack)) {
        write(1, "RADIO_SPWN_FAIL\n", 16);
        s_session_alive.store(false, std::memory_order_release);
        SetStatus(RadioStatus::Failed);
        ReleaseSystemAudio();
        return;
    }
    write(1, "RADIO_START_DONE\n", 17);
}

}  // namespace

RadioPlayer &RadioPlayer::Instance()
{
    static RadioPlayer s_instance;
    return s_instance;
}

void RadioPlayer::Start()
{
    write(1, "RADIO_START\n", 12);
    s_leave.store(false, std::memory_order_relaxed);
    s_want_play.store(true, std::memory_order_relaxed);
    SetStatus(RadioStatus::Connecting);
    /* OccupySystemAudio runs inside SessionMain (worker) — doing it on the
     * LVGL play/load path blue'd after chat (radio_crash.log). */
    EnsureSession();
}

void RadioPlayer::Stop()
{
    write(1, "RADIO_STOP\n", 11);
    s_want_play.store(false, std::memory_order_relaxed);
    s_leave.store(true, std::memory_order_relaxed);
    ClearBars();
    RequestAspStop();
}

void RadioPlayer::AbortConnectTimeout()
{
    if (static_cast<RadioStatus>(s_status.load(std::memory_order_relaxed)) !=
        RadioStatus::Connecting) {
        return;
    }
    if (!s_want_play.load(std::memory_order_relaxed)) {
        return;
    }
    if (s_reported_playing.load(std::memory_order_relaxed)) {
        return;
    }
    write(1, "RADIO_TIMEOUT\n", 14);
    s_want_play.store(false, std::memory_order_relaxed);
    SetStatus(RadioStatus::Failed);
    RequestAspStop();
}

void RadioPlayer::Pause()
{
    if (!s_session_alive.load(std::memory_order_relaxed)) {
        return;
    }
    if (!s_want_play.load(std::memory_order_relaxed)) {
        return;
    }
    s_reported_playing.store(false, std::memory_order_relaxed);
    s_want_play.store(false, std::memory_order_relaxed);
    SetStatus(RadioStatus::Paused);
    esp_asp_handle_t handle = s_player;
    if (handle != nullptr) {
        esp_audio_simple_player_pause(handle);
    }
}

void RadioPlayer::Resume()
{
    if (!s_session_alive.load(std::memory_order_relaxed)) {
        Start();
        return;
    }
    if (s_leave.load(std::memory_order_relaxed)) {
        Start();
        return;
    }
    if (s_want_play.load(std::memory_order_relaxed)) {
        return;
    }
    s_reported_playing.store(false, std::memory_order_relaxed);
    s_want_play.store(true, std::memory_order_relaxed);
    SetStatus(RadioStatus::Connecting);
    esp_asp_handle_t handle = s_player;
    if (handle != nullptr) {
        esp_audio_simple_player_resume(handle);
    }
}

void RadioPlayer::TogglePlayPause()
{
    if (!s_session_alive.load(std::memory_order_relaxed) ||
        s_leave.load(std::memory_order_relaxed)) {
        Start();
        return;
    }
    if (s_want_play.load(std::memory_order_relaxed)) {
        Pause();
    } else {
        Resume();
    }
}

void RadioPlayer::SelectStation(int index)
{
    index = ClampStationIndex(index);
    const int prev = s_station_index.load(std::memory_order_relaxed);
    if (prev == index) {
        if (s_session_alive.load(std::memory_order_relaxed) &&
            !s_want_play.load(std::memory_order_relaxed) &&
            !s_leave.load(std::memory_order_relaxed)) {
            Resume();
        }
        return;
    }

    s_station_index.store(index, std::memory_order_release);
    write(1, "RADIO_SWITCH\n", 13);
    ClearBars();
    s_reported_playing.store(false, std::memory_order_relaxed);
    s_want_play.store(true, std::memory_order_relaxed);
    s_leave.store(false, std::memory_order_relaxed);
    SetStatus(RadioStatus::Connecting);

    if (s_session_alive.load(std::memory_order_relaxed)) {
        RequestAspStop();
    } else {
        Start();
    }
}

RadioStatus RadioPlayer::status() const
{
    return static_cast<RadioStatus>(s_status.load(std::memory_order_relaxed));
}

bool RadioPlayer::want_play() const
{
    return s_want_play.load(std::memory_order_relaxed);
}

bool RadioPlayer::is_session_running() const
{
    return s_session_alive.load(std::memory_order_relaxed) &&
           !s_leave.load(std::memory_order_relaxed);
}

int RadioPlayer::station_index() const
{
    return ClampStationIndex(s_station_index.load(std::memory_order_relaxed));
}

const RadioStation &RadioPlayer::station() const
{
    return StationAt(station_index());
}

const char *RadioPlayer::stream_url() const
{
    return station().url;
}

void RadioPlayer::CopyBands(uint8_t out[kBandCount]) const
{
    if (out == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_bar_mu);
    std::memcpy(out, s_bar_levels, sizeof(s_bar_levels));
}

int RadioPlayer::volume() const
{
    return s_volume.load(std::memory_order_relaxed);
}

void RadioPlayer::AdjustVolume(int delta)
{
    int vol = s_volume.load(std::memory_order_relaxed) + delta;
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    s_volume.store(vol, std::memory_order_relaxed);
    Application::GetInstance().GetAudioService().SetVolume(vol);
}
