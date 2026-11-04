#include "hi_task_policy.h"
/*
 * SPDX-FileCopyrightText: 2026 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internet radio: HTTP/HTTPS HLS (m3u8) or progressive MP3 via GMF, PCM played
 * through AudioService (never open /dev/audio/pcm0 — that raced LCDC).
 * HLS ring is sized for one CNR TS segment, not the upstream 600 KB default.
 */

#include "radio_player.h"
#include "radio_connect_policy.h"
#include "app_dns.h"

#include "application.h"
#include "audio_service.h"
#include "bluetooth_screen/bluetooth_screen.h"
#include "settings.h"
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_info.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_io.h"
#include "esp_gmf_new_databus.h"
#include "esp_hls_io.h"
#include "esp_http_client.h"
#include "esp_log_shim.h"
#include "media_lib_adapter.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstring>
#include <mutex>
#include <memory>
#include <cstdlib>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

namespace {

constexpr size_t kPlayStack = 28 * 1024;
constexpr size_t kDrainStack = 8 * 1024;
constexpr size_t kHttpStack = 12 * 1024;
constexpr size_t kHlsStack = 12 * 1024;
constexpr size_t kPlayerStack = 16 * 1024;
constexpr size_t kHlsBuf = 300 * 1024;
constexpr size_t kPcmRingSize = 384000; // 24 seconds at 16 kHz, allocated on demand
constexpr size_t kOutChunk = 960;
constexpr int kDecodeBlockFrames = 256;

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
std::atomic<bool> s_need_reopen{false};
// Control requests and asynchronous player events share this short state lock.
// Never hold it while stopping/joining a player or waiting for PCM capacity.
std::mutex s_request_mu;
std::atomic<uint32_t> s_request_id{1};
std::atomic<uint32_t> s_active_id{0};
std::atomic<bool> s_attempt_failed{false};
std::atomic<uint32_t> s_last_submit_ms{0};
std::atomic<uint32_t> s_attempt_start_ms{0}, s_last_decode_ms{0};
std::atomic<int> s_attempt_network{-1};

uint32_t RadioMonotonicMs()
{
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool CurrentAttempt(uint32_t id)
{
    return id != 0 && id == s_request_id.load() && id == s_active_id.load() &&
           s_want_play.load() && !s_leave.load();
}
std::atomic<uint32_t> s_hls_format{0};

std::mutex s_bar_mu;
uint8_t s_bar_levels[RadioPlayer::kBandCount] = {};

pthread_t s_drain_thread{};
bool s_drain_joinable = false;
std::atomic<bool> s_drain_run{false};

esp_asp_handle_t s_player = nullptr;

struct PcmRing {
    int16_t *buf = nullptr; // process-lifetime reusable playback allocation
    uint32_t epoch = 0; // Changes on every reset, including same-station retries.
    bool buffering = true;
    size_t start_threshold = 160000; // two HLS segments plus render/network jitter
    size_t rebuffer_threshold = 192000;
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
};
PcmRing s_pcm = {};
std::atomic<bool> s_segmented{true};
std::mutex s_pcm_mu;
#ifdef HI_STABILITY_PROBE
std::atomic<uint32_t> s_probe_produced{0},s_probe_submitted{0},s_probe_rebuffer{0};
#endif

// Keep interpolation phase across decoder blocks. Resetting at each 256
// input samples drops the fractional remainder and introduces periodic gaps.
class RadioResampler {
public:
    void Configure(int input_rate, int output_rate) {
        input_rate_ = input_rate; output_rate_ = output_rate;
        phase_ = 0; have_previous_ = false;
    }
    // Fixed output. The old vector grew on every decoder block and stalled
    // the panel allocator; a missed scan is a solid blue frame.
    size_t Process(const int16_t *input, size_t count, int16_t *output, size_t cap) {
        size_t n = 0;
        if (output == nullptr || cap == 0 || input_rate_ <= 0 || output_rate_ <= 0)
            return 0;
        for (size_t i = 0; i < count && n < cap; ++i) {
            const int32_t current = input[i];
            if (!have_previous_) {
                output[n++] = static_cast<int16_t>(current);
                previous_ = current; have_previous_ = true; phase_ = input_rate_;
                continue;
            }
            while (phase_ <= output_rate_ && n < cap) {
                const int64_t value = static_cast<int64_t>(previous_) * output_rate_ +
                    static_cast<int64_t>(current - previous_) * phase_;
                output[n++] = static_cast<int16_t>(value / output_rate_);
                phase_ += input_rate_;
            }
            phase_ -= output_rate_;
            previous_ = current;
        }
        return n;
    }
private:
    int input_rate_ = 16000, output_rate_ = 16000;
    int64_t phase_ = 0;
    int32_t previous_ = 0;
    bool have_previous_ = false;
};
RadioResampler s_resampler;
std::atomic<int> s_resampler_rate{0};
// One decoder callback at a time. 6x covers an 8 kHz stream at 16 kHz out.
constexpr size_t kResampleCap = static_cast<size_t>(kDecodeBlockFrames) * 6;
int16_t s_mono_buf[kDecodeBlockFrames];
int16_t s_resampled_buf[kResampleCap];

void SetStatus(RadioStatus kind)
{
    s_status.store(static_cast<int>(kind), std::memory_order_relaxed);
}

int ClampStationIndex(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(kRadioStationCount))
        return kDefaultRadioStationIndex;
    return idx;
}

const RadioStation &StationAt(int idx)
{
    return kRadioStations[static_cast<size_t>(ClampStationIndex(idx))];
}

bool SpawnThread(pthread_t *out, void *(*entry)(void *), void *arg, size_t stack)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_t th = 0;
    const int rc = hi_task_create_legacy(&th, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return false;
    if (out != nullptr)
        *out = th;
    else
        pthread_detach(th);
    return true;
}

// Persist outside LVGL so flash writes do not block touch callbacks.
std::atomic<bool> s_station_save_running{false};
std::atomic<int> s_station_saved{-1};
void SaveStationAsync();
void *SaveStationEntry(void *)
{
    int index;
    do {
        index = s_station_index.load();
        Settings settings("radio", true);
        settings.SetString("last_station", StationAt(index).name);
        s_station_saved.store(index);
    } while (index != s_station_index.load());
    s_station_save_running.store(false);
    if (s_station_saved.load() != s_station_index.load()) SaveStationAsync();
    return nullptr;
}
void SaveStationAsync()
{
    if (s_station_saved.load() == s_station_index.load()) return;
    bool expected = false;
    if (!s_station_save_running.compare_exchange_strong(expected, true)) return;
    if (!SpawnThread(nullptr, SaveStationEntry, nullptr, 8192)) {
        s_station_save_running.store(false);
        write(1, "RADIO_SAVE_DEFERRED\n", 20);
    }
}

void JoinThread(pthread_t *th, bool *joinable)
{
    if (th == nullptr || joinable == nullptr || !*joinable)
        return;
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
    if (samples == nullptr || n == 0)
        return;
    uint8_t levels[RadioPlayer::kBandCount];
    const size_t slice =
        std::max<size_t>(1, n / static_cast<size_t>(RadioPlayer::kBandCount));
    for (int b = 0; b < RadioPlayer::kBandCount; ++b) {
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
        uint32_t mean =
            (end > begin) ? static_cast<uint32_t>(acc / (end - begin)) : 0;
        uint32_t scaled = (mean * 255u) / 6000u;
        if (scaled > 255u)
            scaled = 255u;
        levels[b] = static_cast<uint8_t>(scaled);
    }
    std::lock_guard<std::mutex> lock(s_bar_mu);
    std::memcpy(s_bar_levels, levels, sizeof(s_bar_levels));
}

void PushPcm(const int16_t *samples, size_t n, uint32_t id)
{
    if (samples == nullptr || n == 0)
        return;
    size_t i = 0;
    while (i < n) {
        if (!CurrentAttempt(id))
            return;
        {
            std::lock_guard<std::mutex> lock(s_pcm_mu);
            // Cancellation can happen between the first check and this lock.
            // Never insert an old station's last block after ClearPcm().
            if (!CurrentAttempt(id)) return;
            if (!s_pcm.buf) return;
            const size_t previous = i;
            while (i < n && s_pcm.count < kPcmRingSize) {
                s_pcm.buf[s_pcm.head] = samples[i++];
                s_pcm.head = (s_pcm.head + 1) % kPcmRingSize;
                s_pcm.count++;
#ifdef HI_STABILITY_PROBE
                ++s_probe_produced;
#endif
            }
            if (i != previous) s_last_decode_ms.store(RadioMonotonicMs());
            if (i >= n)
                return;
        }
        usleep(4000);
    }
}

size_t PopPcm(int16_t *out, size_t n, uint32_t &epoch)
{
    std::lock_guard<std::mutex> lock(s_pcm_mu);
    epoch = s_pcm.epoch;
    if (!s_pcm.buf) return 0;
    if (s_pcm.buffering) {
        if (s_pcm.count < s_pcm.start_threshold) return 0;
        s_pcm.buffering = false;
    }
    // Queue capacity is measured in frames, so never fill it with tiny
    // decoder fragments. Leave a partial frame here until it is complete.
    if (s_pcm.count < n) {
        // Once the reserve is exhausted, rebuild it instead of playing each
        // arriving fragment with another audible gap after every frame.
        s_pcm.buffering = true;
        s_pcm.start_threshold = s_pcm.rebuffer_threshold;
#ifdef HI_STABILITY_PROBE
        ++s_probe_rebuffer;
#endif
        return 0;
    }
    const size_t to_read = n;
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
    ++s_pcm.epoch;
    s_pcm.head = s_pcm.tail = s_pcm.count = 0;
    s_pcm.buffering = true;
    s_pcm.start_threshold = s_segmented.load() ? 160000 : 64000;
    s_pcm.rebuffer_threshold = s_segmented.load() ? 192000 : 96000;
}

int OutCallback(uint8_t *data, int data_size, void *ctx)
{
    const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    // Drop superseded PCM while acknowledging the write. Returning zero
    // makes GMF retry the canceled output and can block station teardown.
    if (!CurrentAttempt(id))
        return data_size > 0 ? data_size : 0;
    if (data == nullptr || data_size <= 0)
        return 0;
    const int consumed = data_size;

    const int16_t *pcm = reinterpret_cast<const int16_t *>(data);
    const int total_samples = data_size / static_cast<int>(sizeof(int16_t));
    const int channels = s_pcm_channels.load(std::memory_order_relaxed);
    if (channels <= 0 || total_samples <= 0)
        return 0;
    const int frames = total_samples / channels;
    if (frames <= 0)
        return 0;

    const int src_rate = s_pcm_sample_rate.load(std::memory_order_relaxed);
    int frame_off = 0;
    while (frame_off < frames) {
        if (!CurrentAttempt(id))
            return consumed;
        const int block = std::min(frames - frame_off, kDecodeBlockFrames);
        for (int i = 0; i < block; ++i) {
            int32_t mix = 0;
            const int src = (frame_off + i) * channels;
            for (int c = 0; c < channels; ++c)
                mix += pcm[src + c];
            s_mono_buf[i] = static_cast<int16_t>(mix / channels);
        }
        const int16_t *out = s_mono_buf;
        size_t out_n = static_cast<size_t>(block);
        if (src_rate > 0 && src_rate != RadioPlayer::kSampleRate) {
            if (s_resampler_rate.load(std::memory_order_relaxed) != src_rate) {
                s_resampler.Configure(src_rate, RadioPlayer::kSampleRate);
                s_resampler_rate.store(src_rate, std::memory_order_relaxed);
            }
            out_n = s_resampler.Process(s_mono_buf, static_cast<size_t>(block),
                                        s_resampled_buf, kResampleCap);
            out = s_resampled_buf;
        }
        PushPcm(out, out_n, id);
        frame_off += block;
    }
    /* GMF treats 0 as "wrote nothing" and stalls — that looked like
     * 正在播放 with silence. Echo the input size we accepted. */
    return consumed;
}

int EventCallback(esp_asp_event_pkt_t *pkt, void *ctx)
{
    const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    std::lock_guard<std::mutex> lock(s_request_mu);
    if (!CurrentAttempt(id)) return 0;
    if (pkt == nullptr)
        return 0;
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE && pkt->payload != nullptr &&
        pkt->payload_size >= static_cast<int>(sizeof(esp_asp_state_t))) {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        std::memcpy(&state, pkt->payload, sizeof(state));
        // A live station finishing is not a usable silent Playing state.
        if (state == ESP_ASP_STATE_ERROR || state == ESP_ASP_STATE_FINISHED) {
            s_attempt_failed.store(true);
        }
    } else if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
               pkt->payload != nullptr &&
               pkt->payload_size >=
                   static_cast<int>(sizeof(esp_asp_music_info_t))) {
        esp_asp_music_info_t info = {};
        std::memcpy(&info, pkt->payload, sizeof(info));
        if (info.channels > 0)
            s_pcm_channels.store(info.channels, std::memory_order_relaxed);
        if (info.sample_rate > 0)
            s_pcm_sample_rate.store(info.sample_rate, std::memory_order_relaxed);
        write(1, "RADIO_INFO\n", 11);
    }
    return 0;
}

int HlsSegCb(esp_hls_file_seg_info_t *info, void *ctx)
{
    if (info == nullptr || info->format == 0)
        return 0;
    const uint32_t prev = s_hls_format.load(std::memory_order_relaxed);
    if (prev == info->format)
        return 0;
    s_hls_format.store(info->format, std::memory_order_relaxed);
    auto player = static_cast<esp_asp_handle_t>(ctx);
    if (player == nullptr)
        return 0;
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    if (esp_audio_simple_player_get_pipeline(player, &pipeline) !=
            ESP_GMF_ERR_OK ||
        pipeline == nullptr)
        return 0;
    esp_gmf_element_handle_t dec_el = nullptr;
    if (esp_gmf_pipeline_get_el_by_name(pipeline, "aud_dec", &dec_el) !=
            ESP_GMF_ERR_OK ||
        dec_el == nullptr)
        return 0;
    esp_gmf_info_sound_t music_info = {};
    music_info.format_id = info->format;
    music_info.sample_rates = s_pcm_sample_rate.load(std::memory_order_relaxed);
    music_info.channels =
        static_cast<uint16_t>(s_pcm_channels.load(std::memory_order_relaxed));
    music_info.bits = 16;
    (void)esp_gmf_audio_dec_reconfig_by_sound_info(dec_el, &music_info);
    write(1, "RADIO_HLS\n", 10);
    return 0;
}

int HttpHook(http_stream_event_msg_t *msg)
{
    if (msg == nullptr || msg->http_client == nullptr)
        return ESP_GMF_ERR_OK;
    const uint32_t id=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(msg->user_data));
    if (!CurrentAttempt(id)) return ESP_GMF_ERR_FAIL;
    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        auto client = static_cast<esp_http_client_handle_t>(msg->http_client);
        esp_http_client_set_cancel_cb(client, [](void *ctx) -> bool {
            const uint32_t request = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
            return !CurrentAttempt(request) || s_attempt_failed.load() ||
                   s_need_reopen.load() || s_attempt_network.load() != metalio_app_cellular_active();
        }, msg->user_data);
        // Limit only radio's cellular TCP packets. Large Ethernet bursts on
        // the UART-backed modem lost partial frames and starved later DNS.
        // Other HTTP clients and Wi-Fi retain their original TCP defaults.
        if (esp_http_client_set_transport_mss(client, metalio_app_cellular_active() ? 512 : 0) != ESP_OK)
            return ESP_GMF_ERR_FAIL;
        esp_http_client_set_header(client, "User-Agent",
                                   "Mozilla/5.0 (HiOS Radio)");
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        esp_http_client_set_header(client, "Accept", "*/*");
        esp_http_client_set_header(client, "Connection", "keep-alive");
    }
    return ESP_GMF_ERR_OK;
}

void EnsureMediaLibAdapter()
{
    if (s_media_lib_ready.load(std::memory_order_relaxed))
        return;
    media_lib_add_default_adapter();
    s_media_lib_ready.store(true, std::memory_order_relaxed);
}

esp_gmf_err_t HttpTlsGetScore(esp_gmf_io_handle_t /*handle*/, const char *url,
                              int *score)
{
    *score = ESP_GMF_IO_SCORE_NONE;
    if (url == nullptr)
        return ESP_GMF_ERR_OK;
    /* HLS demuxer owns .m3u8; progressive http(s):// uses esp_gmf_io_http. */
    if (strstr(url, ".m3u8") != nullptr || strstr(url, ".M3U8") != nullptr)
        return ESP_GMF_ERR_OK;
    if (strncasecmp(url, "https://", 8) == 0 ||
        strncasecmp(url, "http://", 7) == 0) {
        *score = ESP_GMF_IO_SCORE_STANDARD + 10;
    }
    return ESP_GMF_ERR_OK;
}

bool RegisterHttp(esp_asp_handle_t player, uint32_t id)
{
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.crt_bundle_attach = nullptr;
    http_cfg.event_handle = HttpHook;
    http_cfg.user_data = reinterpret_cast<void *>(static_cast<uintptr_t>(id));
    http_cfg.io_cfg.thread.stack = kHttpStack;
    http_cfg.io_cfg.thread.prio = 5;
    http_cfg.io_cfg.thread.stack_in_ext = true;
    http_cfg.io_cfg.buffer_cfg.buffer_size = 96 * 1024;
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

bool RegisterHls(esp_asp_handle_t player)
{
    esp_gmf_pool_handle_t pool = nullptr;
    if (esp_audio_simple_player_get_pool(player, &pool) != ESP_GMF_ERR_OK ||
        pool == nullptr) {
        write(1, "RADIO_HLS_POOL\n", 15);
        return false;
    }
    esp_hls_io_cfg_t hls_cfg = {};
    hls_cfg.name = "io_hls";
    hls_cfg.file_seg_cb = HlsSegCb;
    hls_cfg.ctx = player;
    hls_cfg.pool = pool;
    hls_cfg.io_cfg.thread.stack = kHlsStack;
    hls_cfg.io_cfg.thread.prio = 5;
    hls_cfg.io_cfg.thread.core = 0;
    hls_cfg.io_cfg.thread.stack_in_ext = true;
    hls_cfg.io_cfg.buffer_cfg.io_size = 512;
    hls_cfg.io_cfg.buffer_cfg.buffer_size = kHlsBuf;
    esp_gmf_io_handle_t hls_io = nullptr;
    if (esp_gmf_io_hls_init(&hls_cfg, &hls_io) != ESP_GMF_ERR_OK ||
        hls_io == nullptr) {
        write(1, "RADIO_HLS_FAIL\n", 15);
        return false;
    }
    if (esp_audio_simple_player_register_io(player, hls_io) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(hls_io);
        return false;
    }
    return true;
}

void DestroyPlayer()
{
    if (s_player == nullptr)
        return;
    { std::lock_guard<std::mutex> lock(s_request_mu); s_active_id.store(0); }
    esp_asp_handle_t player = s_player;
    s_player = nullptr;
    esp_audio_simple_player_stop(player);
    esp_audio_simple_player_destroy(player);
}

esp_gmf_err_t AbortRadioInput(void *ctx)
{
    esp_gmf_io_handle_t input = nullptr;
    auto pipeline = static_cast<esp_gmf_pipeline_handle_t>(ctx);
    if (esp_gmf_pipeline_get_in(pipeline, &input) != ESP_GMF_ERR_OK || !input)
        return ESP_GMF_ERR_OK;
    auto *io = reinterpret_cast<esp_gmf_io_t *>(input);
    // Wake the decoder BEFORE asking its task to stop. Previously stop
    // waited for a task blocked on this bus, while closing/aborting the bus
    // was queued behind that very same task: an indefinite teardown wait.
    if (io->data_bus) esp_gmf_db_abort(io->data_bus);
    if (io->prev_close) io->prev_close(io);
    return ESP_GMF_ERR_OK;
}

int PrepareRadioPipeline(esp_asp_handle_t *, void *ctx)
{
    const auto player = *static_cast<esp_asp_handle_t *>(ctx);
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    const auto rc = esp_audio_simple_player_get_pipeline(player, &pipeline);
    if (rc != ESP_GMF_ERR_OK || !pipeline) return ESP_GMF_ERR_FAIL;
    return esp_gmf_pipeline_set_prev_stop_cb(pipeline, AbortRadioInput, pipeline);
}

bool CreatePlayer(uint32_t id)
{
    DestroyPlayer();
    s_pcm_channels.store(2, std::memory_order_relaxed);
    s_pcm_sample_rate.store(44100, std::memory_order_relaxed);
    s_resampler_rate.store(0, std::memory_order_relaxed);
    s_hls_format.store(0, std::memory_order_relaxed);
    esp_asp_cfg_t cfg = {};
    cfg.out.cb = OutCallback;
    cfg.out.user_ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(id));
    cfg.task_prio = 5;
    cfg.task_stack = kPlayerStack;
    cfg.task_core = hi_task_supported_cpus() > 1 ? 1 : 0;
    cfg.prev = PrepareRadioPipeline;
    cfg.prev_ctx = &s_player;
    if (esp_audio_simple_player_new(&cfg, &s_player) != ESP_GMF_ERR_OK ||
        s_player == nullptr) {
        write(1, "RADIO_NEW_FAIL\n", 15);
        return false;
    }
    if (!RegisterHttp(s_player, id) || !RegisterHls(s_player)) {
        DestroyPlayer();
        return false;
    }
    esp_audio_simple_player_set_event(s_player, EventCallback, cfg.out.user_ctx);
    return true;
}

void *PcmDrainEntry(void * /*arg*/)
{

    int policy=0;sched_param priority{};
    if(pthread_getschedparam(pthread_self(),&policy,&priority)==0){
        priority.sched_priority=std::max(priority.sched_priority,CONFIG_METALIO_APP_PRIORITY+12);
        (void)pthread_setschedparam(pthread_self(),SCHED_RR,&priority);
    }
    int16_t chunk[kOutChunk];
    uint32_t session=0, request=0, epoch=0;
    uint32_t watchdog_ms = RadioMonotonicMs();
    auto &as = Application::GetInstance().GetAudioService();
    const auto check_progress = [&watchdog_ms]() {
        const uint32_t now = RadioMonotonicMs();
        if (now - watchdog_ms >= 200) {
            watchdog_ms = now;
            // Connection recovery must run even when LVGL is busy or the
            // radio is covered by the standby clock.
            RadioPlayer::Instance().AbortConnectTimeout();
        }
    };
    while (s_drain_run.load(std::memory_order_relaxed)) {
        check_progress();
        if (!s_want_play.load(std::memory_order_relaxed) ||
            s_leave.load(std::memory_order_relaxed)) {
            if(session){as.EndLocalPlayback(session);session=0;}
            usleep(12000);
            continue;
        }
        const uint32_t current=s_request_id.load();
        if(request!=current){if(session)as.EndLocalPlayback(session);session=0;request=current;}
        uint32_t chunk_epoch=0;
        const size_t got = PopPcm(chunk, kOutChunk, chunk_epoch);
        if(epoch!=chunk_epoch){if(session)as.EndLocalPlayback(session);session=0;epoch=chunk_epoch;}
        if (got == 0) {
            usleep(8000);
            continue;
        }
        if (!CurrentAttempt(request)) continue;
        if(!session)session=as.BeginLocalPlayback(5760);
        UpdateBandsFromPcm(chunk, got);
        /* The shared AudioService queue is bounded. Retrying here applies
         * backpressure instead of silently dropping every block while the
         * I2S writer is busy, which left the UI reporting Playing without
         * continuous audible output. */
        bool submitted=false;
        while (s_drain_run.load(std::memory_order_relaxed) &&
               s_want_play.load(std::memory_order_relaxed) &&
               !s_leave.load(std::memory_order_relaxed) &&
               CurrentAttempt(request)) {
            {
                std::lock_guard<std::mutex> lock(s_pcm_mu);
                if(epoch!=s_pcm.epoch || !CurrentAttempt(request)) break;
                submitted=as.TryEnqueueLocalPcm(session, chunk, got);
            }
            if(submitted) {
                s_last_submit_ms.store(RadioMonotonicMs());
#ifdef HI_STABILITY_PROBE
                s_probe_submitted.fetch_add(got);
#endif
                break;
            }
            check_progress();
            usleep(5000);
        }
        {
            std::lock_guard<std::mutex> lock(s_request_mu);
            if(submitted && CurrentAttempt(request) && !s_reported_playing.exchange(true)) {
                write(1,"RADIO_PCM\n",10);
                SetStatus(RadioStatus::Playing);
            }
        }
        // The I2S writer is the clock. Queue backpressure already paces us;
        // sleeping for another block duration here drains the jitter reserve
        // and adds a gap after every scheduling/network delay.
    }
    if(session)as.EndLocalPlayback(session);
    return nullptr;
}

void StartDrain()
{
    if (!s_pcm.buf) s_pcm.buf = static_cast<int16_t *>(std::malloc(kPcmRingSize * sizeof(int16_t)));
    if (!s_pcm.buf) { s_want_play.store(false); return; }
    s_drain_run.store(false, std::memory_order_release);
    JoinThread(&s_drain_thread, &s_drain_joinable);
    ClearPcm();
    s_drain_run.store(true, std::memory_order_release);
    if (!SpawnThread(&s_drain_thread, PcmDrainEntry, nullptr, kDrainStack)) {
        s_drain_run.store(false, std::memory_order_relaxed);
        return;
    }
    s_drain_joinable = true;
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
    if (s_audio_occupied.exchange(true, std::memory_order_acq_rel))
        return;
    /* Do not ArmDownlink / MUTE / open pcm0 here. Radio PCM goes through
     * the existing AudioService playback queue (same I2S as TTS). Exclusive
     * arming stole the voice TX path and left radio "Playing" with TX off. */
    // Playback never changes the system volume, including mute.
}

void ReleaseSystemAudio()
{
    s_audio_occupied.store(false, std::memory_order_release);
    auto &as = Application::GetInstance().GetAudioService();
    as.ClearDownlinkPlaybackProtect();
    as.ClearWakeWordMute();
}

void EnsureSession();

void *SessionEntry(void * /*arg*/)
{
    OccupySystemAudio();
    EnsureMediaLibAdapter();
    if(BluetoothScreen::PreparePlaybackRoute()<0){
        s_want_play=false;SetStatus(RadioStatus::Failed);
        ReleaseSystemAudio();s_session_alive=false;return nullptr;
    }
    StartDrain();
    write(1, "RADIO_PLAY\n", 11);

    uint32_t fallback_request = 0, retry_request = 0;
    unsigned retry_count = 0;
    while (!s_leave.load()) {
        uint32_t id;
        int playing_station;
        {
            std::lock_guard<std::mutex> lock(s_request_mu);
            id = s_request_id.load();
            playing_station = s_station_index.load();
            s_need_reopen.store(false);
        }
        if (!s_want_play.load()) { usleep(20000); continue; }
        if (retry_request != id) { retry_request = id; retry_count = 0; }
        ClearPcm();
        const auto &selected = StationAt(playing_station);
        const char *url = fallback_request == id && selected.fallback_url
                            ? selected.fallback_url : selected.url;
        s_segmented.store(strstr(url,".m3u8")!=nullptr || strstr(url,".M3U8")!=nullptr);
        ClearPcm();
        const bool created = CreatePlayer(id);
        bool run = false;
        {
            std::lock_guard<std::mutex> lock(s_request_mu);
            if (id == s_request_id.load() && s_want_play.load() && !s_leave.load()) {
                s_active_id.store(id);
                s_attempt_failed.store(!created);
                s_reported_playing.store(false);
                const uint32_t now = RadioMonotonicMs();
                s_attempt_start_ms.store(now);
                s_last_submit_ms.store(now);
                s_last_decode_ms.store(now);
                s_attempt_network.store(metalio_app_cellular_active());
                SetStatus(RadioStatus::Connecting);
                run = created;
            }
        }
        if (run) {
            write(1, "RADIO_RUN\n", 10);
            const int rc = esp_audio_simple_player_run(s_player, url, nullptr);
            if (rc != ESP_GMF_ERR_OK) {
                std::lock_guard<std::mutex> lock(s_request_mu);
                if (CurrentAttempt(id)) s_attempt_failed.store(true);
            }
            while (CurrentAttempt(id) && !s_attempt_failed.load() && !s_need_reopen.load())
                usleep(80000);
        }
        // Invalidate events BEFORE stop: cancellation errors belong to the old stream.
        const bool failed = s_attempt_failed.load();
        DestroyPlayer();
        ClearPcm();
        {
            std::lock_guard<std::mutex> lock(s_request_mu);
            if (id == s_request_id.load() && s_want_play.load() && !s_leave.load() && failed) {
                if (fallback_request != id && selected.fallback_url) {
                    // Do not spend another full connection timeout on the
                    // same silent primary before trying its alternate.
                    fallback_request = id;
                    retry_count = 0;
                    SetStatus(RadioStatus::Connecting);
                } else if (retry_count++ == 0) {
                    SetStatus(RadioStatus::Connecting);
                } else {
                    s_want_play.store(false);
                    SetStatus(RadioStatus::Failed);
                    write(1, "RADIO_REQUEST_FAILED\n", 21);
                }
            }
        }
        usleep(50000);
    }

    StopDrain();
    DestroyPlayer();
    ReleaseSystemAudio();
    s_session_alive.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> lock(s_request_mu);
      if (s_leave.load()) SetStatus(RadioStatus::Idle); }
    write(1, "RADIO_END\n", 10);
    // A new start may arrive while the previous worker is releasing audio.
    if (s_want_play.load() && !s_leave.load()) EnsureSession();
    return nullptr;
}

void EnsureSession()
{
    bool expected = false;
    if (!s_session_alive.compare_exchange_strong(expected, true)) return;
    if (!SpawnThread(nullptr, SessionEntry, nullptr, kPlayStack)) {
        s_session_alive.store(false);
        SetStatus(RadioStatus::Failed);
        s_want_play.store(false, std::memory_order_relaxed);
    }
}

}  // namespace

RadioPlayer::RadioPlayer()
{
    write(1, "RADIO_RESTORE_BEGIN\n", 20);
    Settings settings("radio");
    write(1, "RADIO_RESTORE_READ\n", 19);
    const auto saved = settings.GetString("last_station");
    write(1, "RADIO_RESTORE_DONE\n", 19);
    for (size_t i = 0; i < kRadioStationCount; ++i) {
        if (saved == kRadioStations[i].name) {
            s_station_index.store(static_cast<int>(i));
            s_station_saved.store(static_cast<int>(i));
            break;
        }
    }
}

RadioPlayer &RadioPlayer::Instance()
{
    static RadioPlayer s_instance;
    return s_instance;
}

void RadioPlayer::Start()
{
    std::lock_guard<std::mutex> lock(s_request_mu);
    ++s_request_id;
    write(1, "RADIO_START\n", 12);
    s_leave.store(false, std::memory_order_relaxed);
    s_want_play.store(true, std::memory_order_relaxed);
    s_reported_playing.store(false, std::memory_order_relaxed);
    SetStatus(RadioStatus::Connecting);
    EnsureSession();
}

void RadioPlayer::Stop()
{
    std::lock_guard<std::mutex> lock(s_request_mu);
    ++s_request_id;
    write(1, "RADIO_STOP\n", 11);
    SaveStationAsync();
    s_want_play.store(false, std::memory_order_relaxed);
    s_leave.store(true, std::memory_order_relaxed);
    ClearBars();
    // The session worker owns stop/destroy; UI callbacks never race its handle.
}

void RadioPlayer::AbortConnectTimeout()
{
    std::lock_guard<std::mutex> lock(s_request_mu);
    if (!CurrentAttempt(s_request_id.load()) || s_attempt_failed.load())
        return;
    const int network = metalio_app_cellular_active();
    if (network != s_attempt_network.load()) {
        s_need_reopen.store(true);
        SetStatus(RadioStatus::Connecting);
        return;
    }
    const bool playing = s_reported_playing.load();
    const bool expired = RadioProgressExpired(network != 0, playing, RadioMonotonicMs(),
        s_attempt_start_ms.load(), s_last_decode_ms.load(), s_last_submit_ms.load());
    if (!expired) return;
    // The worker tries the alternate source before publishing terminal failure.
    s_attempt_failed.store(true);
    write(1, "RADIO_PROGRESS_TIMEOUT\n", sizeof("RADIO_PROGRESS_TIMEOUT\n") - 1);
}

void RadioPlayer::Pause()
{
    std::lock_guard<std::mutex> lock(s_request_mu);
    ++s_request_id;
    if (!s_session_alive.load())
        return;
    s_want_play.store(false, std::memory_order_relaxed);
    s_reported_playing.store(false, std::memory_order_relaxed);
    SetStatus(RadioStatus::Paused);
    ClearBars();
}

void RadioPlayer::Resume()
{
    Start();
}

void RadioPlayer::TogglePlayPause()
{
    if (!s_session_alive.load() || s_leave.load()) {
        Start();
        return;
    }
    if (s_want_play.load())
        Pause();
    else
        Resume();
}

void RadioPlayer::SelectStation(int index)
{
    std::lock_guard<std::mutex> lock(s_request_mu);
    ++s_request_id;
    index = ClampStationIndex(index);
    s_station_index.store(index, std::memory_order_release);
    ClearPcm();
    SaveStationAsync();
    s_reported_playing.store(false, std::memory_order_relaxed);
    s_want_play.store(true, std::memory_order_relaxed);
    s_leave.store(false, std::memory_order_relaxed);
    SetStatus(RadioStatus::Connecting);
    if (s_session_alive.load()) {
        s_need_reopen.store(true, std::memory_order_relaxed);
        // The session worker owns stop/destroy; UI callbacks never race its handle.
    } else {
        EnsureSession();
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
    if (out == nullptr)
        return;
    std::lock_guard<std::mutex> lock(s_bar_mu);
    std::memcpy(out, s_bar_levels, sizeof(s_bar_levels));
}

int RadioPlayer::volume() const
{
    return Application::GetInstance().GetAudioService().GetVolume();
}

void RadioPlayer::AdjustVolume(int delta)
{
    int vol = volume() + delta;
    if (vol < 0)
        vol = 0;
    if (vol > 100)
        vol = 100;
    Application::GetInstance().GetAudioService().SetVolume(vol, false);
}

#ifdef HI_STABILITY_PROBE
RadioPlayer::ProbeStats RadioPlayer::GetProbeStats() const {
    std::lock_guard<std::mutex> lock(s_pcm_mu);
    return {s_probe_produced.load(),s_probe_submitted.load(),s_probe_rebuffer.load(),s_pcm.count};
}
#endif
