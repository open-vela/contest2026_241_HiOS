#include "hi_task_cpp.h"
/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * AudioService implementation for NuttX/openvela.
 *
 * Ported from the ESP-IDF MetalioClaw4 audio_service.cc.  Key changes:
 *
 *   - FreeRTOS xTaskCreate -> std::thread (pthread)
 *   - FreeRTOS event groups -> std::atomic<uint32_t>
 *   - vTaskDelay -> usleep / std::this_thread::sleep_for
 *   - esp_timer -> esp_timer_shim (NuttX work_queue)
 *   - Board::GetInstance().GetAudioCodec() -> stored codec_ pointer
 *   - Opus encode/decode -> stub wrappers (passthrough PCM)
 *
 * See audio_service.h for the full data-flow diagram.
 */

#include "audio_service.h"
#include "metalio_audio_codec.h"
#include "energy_wake_word.h"
#include "wake_words/hi_openvela_command.h"
#if defined(CONFIG_METALIO_ESP_SR)
#include "wake_words/hi_openvela_wake_word.h"
#endif
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "board_shim.h"
#include "display.h"
#include <metalio/metalio.h>

#ifdef CONFIG_METALIO_USE_AFE_WAKE_WORD
#include "afe_wake_word.h"
#include "esp_wake_word.h"
#endif
#ifdef CONFIG_METALIO_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h"
#endif
#ifdef CONFIG_METALIO_ESP_SR
#include "metalio_srmodel_boot.h"
#include "model_path.h"
#include <esp_wn_models.h>
#endif

#include <cstring>
#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <nuttx/config.h>
#include <thread>
#include <chrono>

#define TAG "AudioService"

namespace {
// The first greeting competes with initial LVGL/font/TLS work. Boost only
// these bounded audio jobs, then restore their original scheduling settings.
class WakeGreetingPriority {
public:
    WakeGreetingPriority()
    {
        valid_ = pthread_getschedparam(pthread_self(), &policy_, &original_) == 0;
    }
    ~WakeGreetingPriority() { Update(false, 0); }
    void Update(bool greeting, int priority)
    {
        if (!valid_ || (greeting == boosted_ && (!greeting || applied_ == std::max(priority,original_.sched_priority))))
            return;
        if (greeting)
        {
            sched_param target = original_;
            if (target.sched_priority < priority)
                target.sched_priority = priority;
            if (pthread_setschedparam(pthread_self(), SCHED_RR, &target) == 0){boosted_ = true;applied_=target.sched_priority;}
        }
        else if (pthread_setschedparam(pthread_self(), policy_, &original_) == 0)
            boosted_ = false;
    }
private:
    int policy_ = 0;
    sched_param original_{};
    bool valid_ = false;
    bool boosted_ = false;
    int applied_=0;
};

uint64_t AudioNowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

/* ------------------------------------------------------------------ */
/* Construction / destruction                                         */
/* ------------------------------------------------------------------ */

AudioService::AudioService()
{
}

AudioService::~AudioService()
{
    Stop();

    if (audio_power_timer_ != nullptr)
    {
        esp_timer_stop(audio_power_timer_);
        esp_timer_delete(audio_power_timer_);
        audio_power_timer_ = nullptr;
    }
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

void AudioService::CreateDefaultCodec()
{
    /* The MetalioClaw4 board uses direct I2S audio (no I2C codec chip).
     * The ESP32-P4 I2S0 peripheral connects directly to:
     *   - Speaker amplifier (DOUT=GPIO9, PA enabled by TCA9555 P1.0)
     *   - Microphone (DIN=GPIO11)
     *   - BCLK=GPIO12, WS=GPIO10
     *
     * The NuttX /dev/audio/pcm0 (TX) and /dev/audio/pcm_in0 (RX) devices
     * are registered by board_i2s_init() in the board bringup. */
    owned_codec_ = std::make_unique<MetalioAudioCodec>(
        16000,   /* input_sample_rate  */
        16000);  /* output_sample_rate */
    codec_ = owned_codec_.get();
}

void AudioService::Initialize(AudioCodec* codec)
{
    if (codec != nullptr)
    {
        codec_ = codec;
    }
    else if (codec_ == nullptr)
    {
        CreateDefaultCodec();
    }

    codec_->Start();

    /* Setup the Opus codec wrappers (stubs) */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(
        codec_->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(
        16000, 1, OPUS_FRAME_DURATION_MS);
    /* Higher complexity → clearer uplink for cloud STT (was 4). */
    opus_encoder_->SetComplexity(5);

    /* Configure resamplers if the codec sample rate differs from 16 kHz */
    if (codec_->input_sample_rate() != 16000)
    {
        input_resampler_.Configure(codec_->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec_->input_sample_rate(), 16000);
    }

    /* Create the audio processor.
     * Prefer NoAudioProcessor on this NuttX port: AfeAudioProcessor::Initialize
     * allocates a large AFE graph that races LVGL and solid-blues chat enter /
     * first listen. Opus uplink still works via NoAudioProcessor. */
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#if 0 && defined(CONFIG_METALIO_USE_AUDIO_PROCESSOR)
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#endif

    audio_processor_->OnOutput(
        [this](std::vector<int16_t>&& data)
        {
            PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue,
                                  std::move(data));
        });

    audio_processor_->OnVadStateChange(
        [this](bool speaking)
        {
            voice_detected_ = speaking;
            if (callbacks_.on_vad_change)
                callbacks_.on_vad_change(speaking);
        });

#if defined(CONFIG_METALIO_ESP_SR)
    wake_word_ = std::make_unique<HiOpenvelaWakeWord>();
    using_esp_wake_word_ = true;
#else
    wake_word_ = std::make_unique<StubWakeWord>();
#endif
    wake_word_->OnWakeWordDetected(
        [this](const std::string& wake_word)
        {
            if (wake_word != HI_OPENVELA_PHRASE ||
                !(events_.load() & AS_EVENT_WAKE_WORD_RUNNING))
                return;
            if (ShouldSuppressWakeWord())
            {
                // The model stops after a hit. A suppressed result must not
                // leave the enabled detector permanently disarmed.
                wake_word_->Start();
                return;
            }
            // MultiNet has already stopped itself for this accepted hit.
            // Consume the service flag as well: if the UI/call/preference
            // changes before the app handles the event, Idle must be able
            // to re-arm instead of seeing a permanently false "running".
            events_.fetch_and(~AS_EVENT_WAKE_WORD_RUNNING);
            if (callbacks_.on_wake_word_detected)
                callbacks_.on_wake_word_detected(wake_word);
        });

#if defined(CONFIG_METALIO_ESP_SR)
    if (auto *models = LoadMetalioSrModels())
        SetModelsList(models);
#endif

    /* Create the power-management timer */
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg)
        {
            AudioService* self = (AudioService*)arg;
            self->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .name = "audio_power_timer",
    };
    esp_timer_create(&timer_args, &audio_power_timer_);
}

void AudioService::Start()
{
    write(1, "AS0\n", 4);
    /* If Initialize() was not called, do it with a default codec */
    if (codec_ == nullptr)
        Initialize(nullptr);

    write(1, "AS1\n", 4);
    service_stopped_ = false;
    write(1, "AS1a\n", 5);
    events_.store(0);
    write(1, "AS1b\n", 5);

    write(1, "AS2\n", 4);
    /* Launch worker threads */
    audio_input_thread_ = HiTask::LegacyThread([this]() { AudioInputTask(); });
    write(1, "AS3\n", 4);
    audio_output_thread_ = HiTask::LegacyThread([this]() { AudioOutputTask(); });
    write(1, "AS4\n", 4);
    opus_codec_thread_   = HiTask::LegacyThread([this]() { OpusCodecTask(); });
    write(1, "AS5\n", 4);

    /* Model initialization starts asynchronously when detection is enabled. */
    write(1, "AS6\n", 4);
    /* Defer power-mgmt timer — LPWORK callbacks racing boot has wedged CDC. */
    write(1, "AS7\n", 4);
}

void AudioService::Stop()
{
    if (service_stopped_)
        return;

    if (audio_power_timer_)
        esp_timer_stop(audio_power_timer_);

    service_stopped_ = true;
    events_.fetch_or(AS_EVENT_AUDIO_TESTING_RUNNING |
                     AS_EVENT_WAKE_WORD_RUNNING |
                     AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    /* Wake up all blocked threads */
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_encode_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        audio_queue_cv_.notify_all();
    }

    /* Join threads */
    if (audio_input_thread_.joinable())
        audio_input_thread_.join();
    if (audio_output_thread_.joinable())
        audio_output_thread_.join();
    if (opus_codec_thread_.joinable())
        opus_codec_thread_.join();

    ESP_LOGI(TAG, "AudioService stopped");
}

/* ------------------------------------------------------------------ */
/* Audio data reading                                                 */
/* ------------------------------------------------------------------ */

bool AudioService::ReadAudioData(std::vector<int16_t>& data,
                                  int sample_rate, int samples)
{
    if (!codec_->input_enabled())
    {
        if (audio_power_timer_)
        {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_,
                                     AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        }
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate)
    {
        data.resize(samples * codec_->input_sample_rate() / sample_rate *
                    codec_->input_channels());
        if (!codec_->InputData(data))
            return false;

        if (codec_->input_channels() == 2)
        {
            /* De-interleave mic + reference channels, resample, re-interleave */
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2)
            {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }

            auto resampled_mic = std::vector<int16_t>(
                input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_ref = std::vector<int16_t>(
                reference_resampler_.GetOutputSamples(reference_channel.size()));

            input_resampler_.Process(mic_channel.data(), mic_channel.size(),
                                     resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(),
                                          reference_channel.size(),
                                          resampled_ref.data());

            data.resize(resampled_mic.size() + resampled_ref.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2)
            {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_ref[i];
            }
        }
        else
        {
            auto resampled = std::vector<int16_t>(
                input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(),
                                     resampled.data());
            data = std::move(resampled);
        }
    }
    else
    {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data))
            return false;
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

    return true;
}

/* ------------------------------------------------------------------ */
/* Audio input task (mic capture + feed processors)                   */
/* ------------------------------------------------------------------ */

void AudioService::AudioInputTask()
{
    WakeGreetingPriority visual_priority;
    events_.fetch_or(AS_EVENT_INPUT_TASK_RUNNING);
    ESP_LOGI(TAG, "Audio input task started");

    while (true)
    {
        uint32_t bits = events_.load();
        const bool visual=visual_conversation_.load(std::memory_order_relaxed);
        visual_priority.Update(visual && (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING),
                               CONFIG_METALIO_APP_PRIORITY+18);
        /* Check if any of the "feed" modes are active */
        uint32_t feed_bits = bits & (AS_EVENT_AUDIO_TESTING_RUNNING |
                                     AS_EVENT_WAKE_WORD_RUNNING |
                                     AS_EVENT_AUDIO_PROCESSOR_RUNNING);

        if (service_stopped_)
            break;

        // A file recorder owns each microphone sample. Wake/barge-in polling
        // must not consume alternating frames from the same RX DMA stream.
        if(exclusive_capture_){capture_ack_=capture_request_.load();capture_yielded_=true;usleep(10000);continue;}
        capture_yielded_=false;

        // Preserve the RX driver/DMA while local music owns the speaker.
        // Feeding a recognizer with the speaker echo only wastes CPU here.
        if(HasLocalPlaybackSession()){usleep(20000);continue;}
        if (feed_bits == 0)
        {
            /* Speaking+interrupt: VP/WW are off so feed_bits==0. Must still
             * poll barge here — otherwise on_barge_in never runs. */
            MaybeDetectBargeIn();
            usleep(50 * 1000);
            continue;
        }

        if (audio_input_need_warmup_)
        {
            /* Drain one mic frame without encoding — avoids a blind sleep
             * that ate the start of user speech (识别不全). */
            audio_input_need_warmup_ = false;
            std::vector<int16_t> discard;
            int warm_samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (audio_processor_ && audio_processor_->GetFeedSize() > 0)
                warm_samples = (int)audio_processor_->GetFeedSize();
            (void)ReadAudioData(discard, 16000, warm_samples);
            continue;
        }

        /* Audio testing mode (BOOT button loopback test) */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING)
        {
            if (audio_testing_queue_.size() >=
                AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS)
            {
                ESP_LOGW(TAG, "Audio testing queue full, stopping");
                EnableAudioTesting(false);
                continue;
            }

            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples))
            {
                if (codec_->input_channels() == 2)
                {
                    auto mono = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono.size(); ++i, j += 2)
                        mono[i] = data[j];
                    data = std::move(mono);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue,
                                      std::move(data));
                continue;
            }
        }

        /* Voice uplink has priority over wake-word. If both event bits are
         * set (e.g. a stale WW_ON after listen), prefer the processor so
         * continuous dialogue keeps sending Opus to the server. */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING)
        {
            // Downlink owns this audio session while its PCM is queued or
            // draining. Do not read RX only to discard it afterwards: those
            // unnecessary shared-I2S reads chopped the first spoken reply.
            if (ShouldSuppressUplink() ||
                (IsDownlinkPlaybackProtected() && HasPendingPlayback())) {
                usleep(10000);continue;
            }
            std::vector<int16_t> data;
            int samples = (int)audio_processor_->GetFeedSize();
            if (samples > 0)
            {
                if (ReadAudioData(data, 16000, samples))
                {
                    /* Never uplink while TTS is on the speaker — echo → 自问自答. */
                    if (ShouldSuppressUplink())
                        continue;
                    barge_loud_frames_ = 0;
                    /* Feed interleaved mic+ref into the processor. AFE needs
                     * stereo; NoAudioProcessor de-interleaves to mono itself. */
                    audio_processor_->SetVisualConversation(visual);
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
                usleep(10 * 1000);
                continue;
            }
            usleep(10 * 1000);
            continue;
        }

        /* Wake word feed */
        if (bits & AS_EVENT_WAKE_WORD_RUNNING)
        {
            int samples = 0;
            {
                std::lock_guard<std::mutex> lock(wake_word_mutex_);
                if (wake_word_initialized_ && wake_word_)
                    samples = (int)wake_word_->GetFeedSize();
            }
            if (samples > 0)
            {
                std::vector<int16_t> data;
                if (ReadAudioData(data, 16000, samples))
                {
                    if (ShouldSuppressWakeWord())
                    {
                        // Never join syllables from before and after playback.
                        std::lock_guard<std::mutex> lock(wake_word_mutex_);
                        if (wake_word_initialized_ && wake_word_) wake_word_->Reset();
                        continue;
                    }
                    /* Copy detector under lock, invoke callback unlocked so
                     * OnWakeWordDetected can EnableWakeWord without deadlock. */
                    WakeWord *ww = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(wake_word_mutex_);
                        if (wake_word_initialized_ && wake_word_)
                            ww = wake_word_.get();
                    }
                    if (ww)
                        ww->Feed(data);
                    continue;
                }
            }
            usleep(10 * 1000);
            continue;
        }

        /* Interrupt: infrequent mic poll only (VP stays off during TTS so
         * continuous RX does not chop speaker playback on shared I2S). */
        MaybeDetectBargeIn();
        usleep(50 * 1000);
    }

    events_.fetch_and(~AS_EVENT_INPUT_TASK_RUNNING);
    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::MaybeDetectBargeIn()
{
    if (!IsBargeInEnabled() || !callbacks_.on_barge_in)
        return;
    if (!tts_uplink_gate_.load(std::memory_order_relaxed))
        return;
    /* Allow barge even if playback briefly emptied — waiting for
     * HasPendingPlayback() missed many soft interrupts. */

    const auto now = std::chrono::steady_clock::now();
    if (now < barge_cooldown_until_)
        return;
    /* Slower peeks while TTS plays — reduces shared-I2S chop. */
    if (now - barge_last_poll_ < std::chrono::milliseconds(220))
        return;
    barge_last_poll_ = now;

    /* Short peek — not every Opus frame — keeps TX underruns away. */
    std::vector<int16_t> data;
    const int samples = 16000 * 30 / 1000;
    if (!ReadAudioData(data, 16000, samples) || data.empty())
        return;

    const int ch = (codec_ && codec_->input_channels() > 0)
                       ? codec_->input_channels()
                       : 1;
    const size_t frames = data.size() / (size_t)ch;
    if (frames == 0)
        return;

    double sum_sq = 0.0;
    int32_t peak = 0;
    for (size_t i = 0; i < frames; ++i)
    {
        const int16_t s = data[i * (size_t)ch];
        const int32_t a =
            s < 0 ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
        if (a > peak)
            peak = a;
        sum_sq += static_cast<double>(s) * s;
    }
    const float rms = std::sqrt((float)(sum_sq / (double)frames));

    // Learn initial playback bleed before arming; an uncalibrated fixed
    // floor otherwise treats the first loud speaker syllable as user speech.
    if (now < barge_armed_after_) {
        barge_echo_floor_ = std::max(barge_echo_floor_, rms);
        barge_loud_frames_ = 0;
        return;
    }
    /* Learn TTS bleed floor; barge on near-field speech over speaker. */
    if (rms < barge_echo_floor_ * 1.5f)
        barge_echo_floor_ = barge_echo_floor_ * 0.9f + rms * 0.1f;
    const float thr = std::max(barge_echo_floor_ * 2.8f, 900.0f);
    if (rms >= thr && peak >= 4500)
        barge_loud_frames_++;
    else
        barge_loud_frames_ = 0;

    /* Require two separated loud peeks; a single click is not speech. */
    if (barge_loud_frames_ >= 2)
    {
        barge_loud_frames_ = 0;
        barge_cooldown_until_ = now + std::chrono::milliseconds(800);
        write(1, "BARGE\n", 6);
        if (IsBargeInEnabled() && tts_uplink_gate_.load(std::memory_order_acquire))
            callbacks_.on_barge_in();
    }
}

/* ------------------------------------------------------------------ */
/* Audio output task (speaker playback)                               */
/* ------------------------------------------------------------------ */

void AudioService::AudioOutputTask()
{
    WakeGreetingPriority greeting_priority;

    events_.fetch_or(AS_EVENT_OUTPUT_TASK_RUNNING);
    ESP_LOGI(TAG, "Audio output task started");

    /* Match one Opus frame (60 ms @ 16 kHz). Only bridge gaps AFTER real
     * PCM has started — never pump silence while Listening/armed. Doing so
     * kept the BT PA hot on the shared I2S bus and the mic uplink heard it
     * as 电流杂音 on xiaozhi.me. */
    constexpr int kSilenceSamples = 16000 * OPUS_FRAME_DURATION_MS / 1000;
    static std::vector<int16_t> s_bridge(kSilenceSamples, 0);
    static int16_t s_hold_sample = 0;

    while (true)
    {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        greeting_priority.Update(wake_greeting_active_||local_session_||!audio_playback_queue_.empty(), CONFIG_METALIO_APP_PRIORITY + (wake_greeting_active_?20:local_session_?15:10));
        if (service_stopped_)
            break;

        if (exclusive_capture_.load() || (local_session_&&local_complete_write_)) notification_pcm_.clear();
        if (audio_playback_queue_.empty() && !notification_pcm_.empty() && !wake_greeting_active_) {
            auto notification=std::make_unique<AudioTask>();notification->type=kAudioTaskTypeDecodeToPlaybackQueue;
            notification->pcm.resize(std::min((size_t)960,notification_pcm_.size()),0);
            audio_playback_queue_.push_back(std::move(notification));
        }
        if (!audio_playback_queue_.empty())
        {
            if(local_pause_ms_ && audio_playback_queue_.front()->local_session==local_session_){audio_queue_cv_.wait_for(lock,std::chrono::milliseconds(10));continue;}
            bool prime_voice_output=false;
            if(local_session_&&audio_playback_queue_.front()->local_session==local_session_&&local_buffering_){
                size_t samples=0;for(const auto &q:audio_playback_queue_)if(q->local_session==local_session_)samples+=q->pcm.size();
                if(!local_finished_&&samples<local_prebuffer_){audio_queue_cv_.wait_for(lock,std::chrono::milliseconds(10));continue;}
                local_buffering_=false;
            }
            if (wake_greeting_active_ && !wake_greeting_started_ &&
                !audio_playback_queue_.front()->local_session)
            {
                // Start the deadline at the first decoded PCM, not at the
                // first compressed packet before cold decoder initialization.
                if (wake_greeting_first_packet_==std::chrono::steady_clock::time_point{})
                    wake_greeting_first_packet_=std::chrono::steady_clock::now();
                size_t buffered_samples = 0;
                for (const auto &queued : audio_playback_queue_)
                    buffered_samples += queued->pcm.size();
                const int rate = codec_->output_sample_rate();
                const auto elapsed = std::chrono::steady_clock::now() -
                                     wake_greeting_first_packet_;
                // Five 60 ms frames absorb initial decode/network jitter.
                // Short greetings have a bounded wait, even after tts/stop.
                if (rate > 0 && buffered_samples * 1000 / rate < 300 &&
                    elapsed < std::chrono::milliseconds(300))
                {
                    audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(10));
                    continue;
                }
                wake_greeting_started_ = true;
                prime_voice_output=true;
            }
            auto task = std::move(audio_playback_queue_.front());
            audio_playback_queue_.pop_front();
            if(!notification_pcm_.empty() && !wake_greeting_active_) {
                for(size_t i=0;i<task->pcm.size();++i){if(notification_pcm_.empty())break;
                    int v=task->pcm[i]/2+notification_pcm_.front();notification_pcm_.pop_front();
                    task->pcm[i]=(int16_t)std::max(-32768,std::min(32767,v));
                }
            }
            output_in_progress_ = true;output_local_session_=task->local_session;
            if(task->local_session==local_session_ && task->local_session && !local_clock_start_ms_)local_clock_start_ms_=AudioNowMs();
            // Retry short writes for local media without changing the
            // separate complete_write ownership/notification policy.
            const bool complete_write=(task->local_session && task->local_session==local_session_) ||
                (!task->local_session && wake_greeting_active_);
            audio_queue_cv_.notify_all();
            lock.unlock();

            if (!codec_->output_enabled())
            {
                if (audio_power_timer_)
                {
                    esp_timer_stop(audio_power_timer_);
                    esp_timer_start_periodic(
                        audio_power_timer_,
                        AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
                }
                codec_->EnableOutput(true);
            }

            if (!task->pcm.empty())
                s_hold_sample = task->pcm.back();
            bool written=true;
            if(prime_voice_output) {
                // A private voice lead-in lets the PA/DMA settle without
                // consuming the first real syllables. The microphone is gated
                // above; this is never mixed into recordings or local music.
                std::fill(s_bridge.begin(),s_bridge.end(),0);
                for(int frame=0;frame<3 && written;++frame)
                    written=codec_->OutputDataChecked(s_bridge.data(),s_bridge.size());
            }
            if(complete_write)written=codec_->OutputDataChecked(task->pcm.data(),task->pcm.size()) && written;
            else codec_->OutputData(task->pcm);
            last_output_time_ = std::chrono::steady_clock::now();
            debug_statistics_.playback_count++;
            lock.lock();
            if(written && task->local_session && task->local_session==local_session_)local_clock_samples_+=task->pcm.size();
            if(complete_write && !written && task->local_session && task->local_session==local_session_)local_write_failed_=true;
            if(task->local_session)local_output_tail_until_ms_=AudioNowMs()+500;
            output_in_progress_ = false;output_local_session_=0;
            if(local_session_&&audio_playback_queue_.empty()&&!local_finished_)local_buffering_=true;
            if (wake_greeting_active_)
            {
                // The synchronous writer can leave eight 60 ms DMA buffers
                // queued. Keep capture gated until the last real PCM drains.
                const auto dma_tail = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(500);
                if (dma_tail > wake_greeting_drain_until_)
                    wake_greeting_drain_until_ = dma_tail;
            }
            audio_queue_cv_.notify_all();
            continue;
        }

        const auto idle_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_output_time_)
                .count();

        /* Underrun bridge ONLY while Speaking (tts gate). Hold-fade the
         * last sample instead of hard zeros — zero jumps caused 杂音 clicks
         * between Opus frames. Window 20–180 ms covers MQTT jitter without
         * long hiss after TTS_STOP. */
        const bool speaking_tx =
            tts_uplink_gate_.load(std::memory_order_relaxed);
        if (speaking_tx && (!wake_greeting_active_ || wake_greeting_started_) &&
            !local_session_ && codec_ && codec_->output_enabled() &&
            idle_ms >= 20 && idle_ms < 180)
        {
            int16_t hold = s_hold_sample;
            for (int i = 0; i < kSilenceSamples; ++i)
            {
                hold = static_cast<int16_t>((static_cast<int32_t>(hold) * 15) /
                                            16);
                s_bridge[static_cast<size_t>(i)] = hold;
            }
            s_hold_sample = hold;
            lock.unlock();
            codec_->OutputData(s_bridge);
            continue;
        }

        /* Never TX_OFF. EnableOutput(false) used to drop output_enabled and
         * the idle driver path then stopped the shared I2S (mic died after
         * TTS). AutoStop mutes uplink in software; DMA stays armed. */
        audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(20));
    }

    events_.fetch_and(~AS_EVENT_OUTPUT_TASK_RUNNING);
    ESP_LOGW(TAG, "Audio output task stopped");
}

/* ------------------------------------------------------------------ */
/* Opus codec task (encode / decode)                                  */
/* ------------------------------------------------------------------ */

void AudioService::OpusCodecTask()
{
    WakeGreetingPriority greeting_priority;
    events_.fetch_or(AS_EVENT_CODEC_TASK_RUNNING);
    ESP_LOGI(TAG, "Opus codec task started");

    while (true)
    {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]()
        {
            return service_stopped_ ||
                   (!audio_encode_queue_.empty() &&
                    audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                   (!audio_decode_queue_.empty() &&
                    audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });

        if (service_stopped_)
            break;

        greeting_priority.Update(wake_greeting_active_ || visual_conversation_.load(),
                                 CONFIG_METALIO_APP_PRIORITY + (wake_greeting_active_ ? 15 : 10));

        /* Decode — match Claw4: unlock while Decode/resample so AudioOutputTask
         * can keep feeding I2S (holding the lock here caused TX underrun 杂音). */
        if (!audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE)
        {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            decode_in_progress_ = true;
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate,
                                packet->frame_duration);
            bool ok = opus_decoder_ &&
                      opus_decoder_->Decode(std::move(packet->payload),
                                            task->pcm);
            if (ok)
            {
                if (opus_decoder_->sample_rate() !=
                    codec_->output_sample_rate())
                {
                    int target_size = (int)output_resampler_.GetOutputSamples(
                        task->pcm.size());
                    std::vector<int16_t> resampled(target_size);
                    output_resampler_.Process(task->pcm.data(),
                                              task->pcm.size(),
                                              resampled.data());
                    task->pcm = std::move(resampled);
                }

                lock.lock();
                if(!local_session_&&!exclusive_capture_)audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
            }
            else
            {
                ESP_LOGE(TAG, "Failed to decode audio");
                lock.lock();
            }
            decode_in_progress_ = false;
            audio_queue_cv_.notify_all();
            debug_statistics_.decode_count++;
        }

        /* Encode */
        if (!audio_encode_queue_.empty() &&
            audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE)
        {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            if (!opus_encoder_->Encode(std::move(task->pcm),
                                       packet->payload))
            {
                ESP_LOGE(TAG, "Failed to encode audio");
                static int s_enc_bad;
                if ((++s_enc_bad % 17) == 1)
                    write(1, "ENC_BAD\n", 8);
                lock.lock();
                continue;
            }

            if (task->type == kAudioTaskTypeEncodeToSendQueue)
            {
                {
                    std::lock_guard<std::mutex> lk(audio_queue_mutex_);
                    audio_send_queue_.push_back(std::move(packet));
                }
                static int s_enc_ok;
                if ((++s_enc_ok % 17) == 1)
                    write(1, "ENC_OK\n", 7);
                if (callbacks_.on_send_queue_available)
                    callbacks_.on_send_queue_available();
            }
            else if (task->type == kAudioTaskTypeEncodeToTestingQueue)
            {
                std::lock_guard<std::mutex> lk(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            }
            debug_statistics_.encode_count++;
            lock.lock();
        }
    }

    events_.fetch_and(~AS_EVENT_CODEC_TASK_RUNNING);
    ESP_LOGW(TAG, "Opus codec task stopped");
}

/* ------------------------------------------------------------------ */
/* Queue helpers                                                      */
/* ------------------------------------------------------------------ */

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration)
{
    /* Same as Claw4: called from OpusCodecTask after unlocking the queue
     * mutex so playback can proceed during Decode. */
    if (opus_decoder_ &&
        opus_decoder_->sample_rate() == sample_rate &&
        opus_decoder_->duration_ms() == frame_duration)
        return;

    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(
        sample_rate, 1, frame_duration);

    if (opus_decoder_->sample_rate() != codec_->output_sample_rate())
    {
        ESP_LOGI(TAG, "Resampling audio from %d to %d",
                 opus_decoder_->sample_rate(),
                 codec_->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(),
                                    codec_->output_sample_rate());
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type,
                                         std::vector<int16_t>&& pcm)
{
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);

    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]()
    {
        return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE;
    });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(
    std::unique_ptr<AudioStreamPacket> packet, bool wait)
{
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if(local_session_||exclusive_capture_)return false; // recorder/media owns the route
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE)
    {
        if (wait)
        {
            audio_queue_cv_.wait(lock, [this]()
            {
                return audio_decode_queue_.size() <
                       MAX_DECODE_PACKETS_IN_QUEUE;
            });
        }
        else
        {
            return false;
        }
    }
    if (wake_greeting_active_)
    {
        const auto now = std::chrono::steady_clock::now();
        // Control JSON and UDP audio travel independently. A late final
        // packet must extend the drain grace rather than lose its tail.
        // Packet gaps during streaming must not look like an idle speaker.
        wake_greeting_drain_until_ = std::max(wake_greeting_drain_until_,
                                             now + std::chrono::milliseconds(300));
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue()
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty())
        return nullptr;
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

/* ------------------------------------------------------------------ */
/* Wake word                                                          */
/* ------------------------------------------------------------------ */

void AudioService::EncodeWakeWord()
{
    if (wake_word_)
        wake_word_->EncodeWakeWordData();
}

const std::string& AudioService::GetLastWakeWord() const
{
    if (wake_word_)
        return wake_word_->GetLastDetectedWakeWord();
    static const std::string empty;
    return empty;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket()
{
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_ && wake_word_->GetWakeWordOpus(packet->payload))
    {
        packet->sample_rate = 16000;
        packet->frame_duration = OPUS_FRAME_DURATION_MS;
        return packet;
    }
    return nullptr;
}

bool AudioService::PrepareWakeWordDetection()
{
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    if (wake_word_initialized_)
        return true;
    if (!wake_word_ || !wake_word_->Initialize(codec_, models_list_))
        return false;
    wake_word_initialized_ = true;
    write(1, "WAKE_PREPARED\n", 14);
    return true;
}

void AudioService::EnableWakeWordDetection(bool enable)
{
    if (!wake_word_)
        return;

    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    const bool running =
        (events_.load() & AS_EVENT_WAKE_WORD_RUNNING) != 0;

    if (enable)
    {
        /* Never re-arm EnergyWakeWord while uplink is live — it steals the
         * mic path and collapses xiaozhi dialogue back to Idle. */
        if ((events_.load() & AS_EVENT_AUDIO_PROCESSOR_RUNNING) != 0)
        {
            write(1, "WW_BLOCK\n", 9);
            return;
        }
        if (callbacks_.wake_word_allowed && !callbacks_.wake_word_allowed())
        {
            write(1, "WW_BLOCK\n", 9);
            return;
        }
        if (running && wake_word_initialized_)
        {
            // Start is idempotent for the installed Hi openvela engine.
            // Reconcile the actual model state with the service bit after
            // a consumed/discarded callback, without resetting live context
            // or recreating the model/microphone graph.
            wake_word_->Start();
            return;
        }

        ESP_LOGE(TAG, "Enabling wake word detection (initialized=%d)",
                 wake_word_initialized_ ? 1 : 0);
        write(1, "WW_ON\n", 6);

        if (!wake_word_initialized_)
        {
            /* Prefer Esp if EnsureWakeWordReady already swapped it in.
             * Never construct WakeNet under this lock (15s EW_CR1). */
            if (!wake_word_->Initialize(codec_, models_list_))
            {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                write(1, "WW_FAIL\n", 8);
                return;
            }
            wake_word_initialized_ = true;
            write(1, "WW_INIT\n", 8);
        }
        /* Do NOT EnsureWakeWordReady() here — Esp/WakeNet create racing
         * EnableWakeWord on the chat-enter tick solid-blues this port.
         * Application schedules Esp after chat paint settles. */
        wake_word_->Start();
        events_.fetch_or(AS_EVENT_WAKE_WORD_RUNNING);
        // The model owns its pre-roll. Discarding a capture frame here
        // removed the first syllable when speech began just after re-arm.
        audio_input_need_warmup_ = false;
    }
    else
    {
        if (!running)
            return;

        ESP_LOGE(TAG, "Disabling wake word detection (initialized=%d)",
                 wake_word_initialized_ ? 1 : 0);
        write(1, "WW_OFF\n", 7);
        events_.fetch_and(~AS_EVENT_WAKE_WORD_RUNNING);
        wake_word_->Stop();
    }
}

void AudioService::ReleaseWakeWordEngine()
{
    if (!wake_word_)
        return;

    events_.fetch_and(~AS_EVENT_WAKE_WORD_RUNNING);

    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    wake_word_->Stop();
    if (wake_word_initialized_)
    {
        ESP_LOGI(TAG, "Releasing wake word engine");
        wake_word_->Deinitialize();
        wake_word_initialized_ = false;
    }
}

/* ------------------------------------------------------------------ */
/* Voice processing / audio testing / AEC                             */
/* ------------------------------------------------------------------ */

void AudioService::EnableVoiceProcessing(bool enable)
{
    ESP_LOGD(TAG, "%s voice processing",
             enable ? "Enabling" : "Disabling");
    write(1, enable ? "VP_ON\n" : "VP_OFF\n", enable ? 6 : 7);

    if (enable)
    {
        /* Soft-stop wake feed so the mic path is exclusive to the uplink
         * processor. Do NOT ReleaseWakeWordEngine() here: tearing down and
         * later re-creating EspWakeWord/WakeNet races LVGL and solid-blues
         * chat on this NuttX port. */
        if ((events_.load() & AS_EVENT_WAKE_WORD_RUNNING) != 0)
        {
            events_.fetch_and(~AS_EVENT_WAKE_WORD_RUNNING);
            std::lock_guard<std::mutex> lock(wake_word_mutex_);
            if (wake_word_)
                wake_word_->Stop();
            write(1, "WW_SOFT\n", 8);
        }

        const bool first_processor_init = !audio_processor_initialized_;
        if (!audio_processor_initialized_)
        {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS,
                                          models_list_);
            /* If AFE create failed, fall back so dialogue still works. */
            if (audio_processor_->GetFeedSize() == 0)
            {
                ESP_LOGW(TAG, "AFE processor unavailable — NoAudioProcessor");
                write(1, "AFE_FB\n", 7);
                audio_processor_ = std::make_unique<NoAudioProcessor>();
                audio_processor_->OnOutput(
                    [this](std::vector<int16_t>&& data)
                    {
                        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue,
                                              std::move(data));
                    });
                audio_processor_->OnVadStateChange(
                    [this](bool speaking)
                    {
                        voice_detected_ = speaking;
                        if (callbacks_.on_vad_change)
                            callbacks_.on_vad_change(speaking);
                    });
                audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS,
                                              models_list_);
            }
            audio_processor_initialized_ = true;
        }
        audio_processor_->EnableDeviceAec(device_aec_enabled_);
        /* Never wipe downlink TTS here.  First wake: cloud greeting Opus
         * often arrives around detect/start; ResetDecoder left text-only.
         * Idle/Abort call ResetDecoder explicitly when the session ends. */
        if (!protect_downlink_playback_.load(std::memory_order_relaxed) &&
            !HasPendingPlayback())
            ResetDecoder();
        /* Discard one mic frame only on first processor bring-up. Re-arming
         * Listening after TTS used to drop ~60ms and cut 句首. */
        audio_input_need_warmup_ = first_processor_init;
        if (codec_)
            codec_->EnableInput(true);
        audio_processor_->Start();
        events_.fetch_or(AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
    else
    {
        audio_processor_->Stop();
        events_.fetch_and(~AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable)
{
    ESP_LOGI(TAG, "%s audio testing",
             enable ? "Enabling" : "Disabling");

    if (enable)
    {
        events_.fetch_or(AS_EVENT_AUDIO_TESTING_RUNNING);
    }
    else
    {
        events_.fetch_and(~AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Move testing queue to decode queue for playback */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable)
{
    device_aec_enabled_ = enable;
    if (!audio_processor_initialized_)
    {
        ESP_LOGI(TAG, "device AEC preference %s (deferred)",
                 enable ? "on" : "off");
        return;
    }
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    audio_processor_->EnableDeviceAec(enable);
}

/* ------------------------------------------------------------------ */
/* Callbacks / models                                                 */
/* ------------------------------------------------------------------ */

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks)
{
    callbacks_ = callbacks;
}

void AudioService::SetModelsList(srmodel_list_t* models_list)
{
    models_list_ = models_list;
    // Model creation is deferred to the dedicated recognition worker.
}

void AudioService::EnsureWakeWordReady()
{
    // EnableWakeWordDetection starts the worker without blocking LVGL.
}

bool AudioService::IsAfeWakeWord()
{
    /* True for any real phrase engine (AFE or Esp WakeNet) — callers use
     * this to mean "not mute EnergyWakeWord". */
    return using_afe_wake_word_ || using_esp_wake_word_;
}

void AudioService::SetWakeWordUpgradeAllowed(bool allowed)
{
    wake_word_upgrade_allowed_.store(allowed, std::memory_order_release);
    write(1, allowed ? "WW_UP_OK\n" : "WW_UP_NO\n", allowed ? 9 : 9);
}

bool AudioService::IsWakeWordUpgradeAllowed() const
{
    return wake_word_upgrade_allowed_.load(std::memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* Sound playback (OGG/Opus parser — passthrough with stubs)          */
/* ------------------------------------------------------------------ */

void AudioService::PlaySound(const std::string_view& ogg)
{
    {std::lock_guard<std::mutex> lock(audio_queue_mutex_);
     if(exclusive_capture_||(local_session_&&local_complete_write_))return;}
    if (!codec_->output_enabled())
    {
        if (audio_power_timer_)
        {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_,
                                     AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        }
        codec_->EnableOutput(true);
    }

    /*
     * The reference implementation parses OGG pages and extracts Opus
     * packets for decoding.  With the Opus decoder stubbed, we attempt
     * a simplified path: if the data looks like raw PCM, play it
     * directly; otherwise, treat each OGG page body as an Opus packet
     * and push it to the decode queue (the stub decoder will passthrough
     * the raw bytes as PCM).
     */
    const uint8_t* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();
    size_t offset = 0;

    /* Find OGG pages and extract packets */
    while (offset + 27 <= size)
    {
        /* Find "OggS" sync */
        size_t pos = (size_t)-1;
        for (size_t i = offset; i + 4 <= size; ++i)
        {
            if (buf[i] == 'O' && buf[i + 1] == 'g' &&
                buf[i + 2] == 'g' && buf[i + 3] == 'S')
            {
                pos = i;
                break;
            }
        }
        if (pos == (size_t)-1)
            break;

        offset = pos;
        const uint8_t* page = buf + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > size)
            break;

        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i)
            body_size += page[27 + i];

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > size)
            break;

        /* Parse packets using lacing values */
        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments)
        {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued;
            do
            {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0)
                continue;

            /* Skip OpusHead and OpusTags packets */
            if (pkt_len >= 8 &&
                (std::memcmp(buf + pkt_start, "OpusHead", 8) == 0 ||
                 std::memcmp(buf + pkt_start, "OpusTags", 8) == 0))
                continue;

            /* Push audio packet to decode queue */
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = 16000;
            packet->frame_duration = 60;
            packet->payload.resize(pkt_len);
            std::memcpy(packet->payload.data(), buf + pkt_start, pkt_len);
            PushPacketToDecodeQueue(std::move(packet), true);
        }

        offset = body_off + body_size;
    }
}

/* ------------------------------------------------------------------ */
/* State queries                                                      */
/* ------------------------------------------------------------------ */

void AudioService::OutputPcm(const int16_t* data, size_t samples)
{
    if (codec_ == nullptr || data == nullptr || samples == 0)
        return;

    if (!codec_->output_enabled())
    {
        if (audio_power_timer_)
        {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_,
                                     AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        }
        codec_->EnableOutput(true);
    }

    codec_->OutputData(data, (int)samples);
    last_output_time_ = std::chrono::steady_clock::now();
    debug_statistics_.playback_count++;
}

void AudioService::PlayLocalPcm(const int16_t* data, size_t samples)
{
    {std::lock_guard<std::mutex> lock(audio_queue_mutex_);
     if(exclusive_capture_||(local_session_&&local_complete_write_))return;}
    if (codec_ == nullptr || data == nullptr || samples == 0)
        return;

    const uint32_t pcm_ms =
        static_cast<uint32_t>((samples * 1000u) / 16000u) + 1200u;
    MuteWakeWordForMs(pcm_ms < 2000u ? 2000u : pcm_ms);

    /* Open TX before the chime. A 静音 ioctl latches the BT speaker, so
     * unmute once here (this runs off the LVGL thread) and never again
     * while muted() stays false. Do not AUDIOIOC_STOP — that races LCDC. */
    if (!codec_->output_enabled())
        codec_->EnableOutput(true);
    if (codec_->muted())
        codec_->SetMute(false);
    ArmDownlinkPlaybackProtect();
    const uint64_t hold_until = AudioNowMs() + 12000u;
    uint64_t prev_hold = hold_tx_until_ms_.load(std::memory_order_relaxed);
    while (hold_until > prev_hold &&
           !hold_tx_until_ms_.compare_exchange_weak(
               prev_hold, hold_until, std::memory_order_relaxed)) {
    }
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if(exclusive_capture_||(local_session_&&local_complete_write_))return;
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        audio_queue_cv_.notify_all();
    }

    constexpr size_t kFrame =
        static_cast<size_t>(16000 * OPUS_FRAME_DURATION_MS / 1000);
    size_t off = 0;
    while (off < samples) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this] {
            return exclusive_capture_.load() || (local_session_&&local_complete_write_) || service_stopped_.load() ||
                   audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE;
        });
        if (exclusive_capture_||(local_session_&&local_complete_write_)||service_stopped_.load())
            break;
        const size_t n = std::min(kFrame, samples - off);
        auto task = std::make_unique<AudioTask>();
        task->type = kAudioTaskTypeDecodeToPlaybackQueue;
        task->pcm.assign(data + off, data + off + n);
        task->timestamp = 0;
        audio_playback_queue_.push_back(std::move(task));
        off += n;
        audio_queue_cv_.notify_all();
    }

    /* A hard underrun when the chime queue drains stops the shared I2S
     * and blues the panel. Pad a short silence tail so DMA winds down. */
    {
        std::vector<int16_t> tail(kFrame, 0);
        for (int i = 0; i < 4; ++i) {
            std::unique_lock<std::mutex> lock(audio_queue_mutex_);
            audio_queue_cv_.wait(lock, [this] {
                return exclusive_capture_.load() || (local_session_&&local_complete_write_) || service_stopped_.load() ||
                       audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE;
            });
            if (exclusive_capture_||(local_session_&&local_complete_write_)||service_stopped_.load())
                break;
            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->pcm = tail;
            task->timestamp = 0;
            audio_playback_queue_.push_back(std::move(task));
            audio_queue_cv_.notify_all();
        }
    }

    {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        int spins = 0;
        while (!service_stopped_.load() && !audio_playback_queue_.empty() &&
               spins < 45) {
            audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(20));
            ++spins;
        }
    }
    /* Last frame is still in the codec DMA; hold TX a bit longer. */
    usleep(180 * 1000);
    MuteWakeWordForMs(900);
    ClearDownlinkPlaybackProtect();
}

void AudioService::EnqueuePlaybackPcm(const int16_t* data, size_t samples)
{
    (void)TryEnqueuePlaybackPcm(data, samples);
}

bool AudioService::TryEnqueuePlaybackPcm(const int16_t* data, size_t samples)
{
    if (codec_ == nullptr || data == nullptr || samples == 0)
        return false;

    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if(exclusive_capture_||(local_session_&&local_complete_write_))return false;
    if (audio_playback_queue_.size() >= MAX_PLAYBACK_TASKS_IN_QUEUE)
        return false;
    auto task = std::make_unique<AudioTask>();
    task->type = kAudioTaskTypeDecodeToPlaybackQueue;
    task->pcm.assign(data, data + samples);
    task->timestamp = 0;
    audio_playback_queue_.push_back(std::move(task));
    lock.unlock();

    ArmDownlinkPlaybackProtect();
    const uint64_t hold_until = AudioNowMs() + 3000u;
    uint64_t prev_hold = hold_tx_until_ms_.load(std::memory_order_relaxed);
    while (hold_until > prev_hold &&
           !hold_tx_until_ms_.compare_exchange_weak(
               prev_hold, hold_until, std::memory_order_relaxed)) {
    }

    lock.lock();
    audio_queue_cv_.notify_all();
    return true;
}

uint32_t AudioService::BeginLocalPlayback(size_t prebuffer_samples,bool complete_write)
{
    // Preserve the established media ABI; only recorder playback opts in to
    // the already-held capture lease through the explicit overload below.
    return BeginLocalPlayback(prebuffer_samples,complete_write,false);
}

uint32_t AudioService::BeginLocalPlayback(size_t prebuffer_samples,bool complete_write,bool capture_owner)
{return BeginLocalPlayback(prebuffer_samples,complete_write,capture_owner,false);}

uint32_t AudioService::BeginLocalPlayback(size_t prebuffer_samples,bool complete_write,bool capture_owner,bool require_idle)
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if(require_idle&&(service_stopped_||local_session_||exclusive_capture_||output_in_progress_||!audio_playback_queue_.empty()||!audio_decode_queue_.empty()))return 0;
    // Recorder ownership also covers its file playback. A late media worker
    // cannot replace that session or change its PCM queue after a stop request.
    // Complete writes describe delivery, not ownership. An alarm also uses
    // complete writes, so it must not replace a recorder's live PCM session.
    // Only the recorder that already holds the capture lease opts in.
    if(exclusive_capture_&&(!capture_owner||!complete_write))return 0;
    if(capture_owner&&!exclusive_capture_)return 0;
    if(++local_session_counter_==0)++local_session_counter_;
    local_session_=local_session_counter_;local_prebuffer_=std::min<size_t>(prebuffer_samples,960*10);
    local_clock_samples_=0;local_clock_start_ms_=0;local_pause_ms_=0;
    local_buffering_=true;local_finished_=false;
    local_complete_write_=complete_write;local_write_failed_=false;
    wake_greeting_active_=false;wake_greeting_started_=false;wake_greeting_ended_=false;
    wake_greeting_first_packet_={};wake_greeting_drain_until_={};
    if(complete_write){notification_pcm_.clear();++notification_epoch_;}
    audio_playback_queue_.clear();audio_decode_queue_.clear();
    audio_queue_cv_.notify_all();return local_session_;
}

bool AudioService::BeginExclusiveCapture()
{
    if(service_stopped_||exclusive_capture_.exchange(true))return false;
    const uint32_t request=capture_request_.fetch_add(1)+1;
    for(int waited=0;waited<300&&!service_stopped_;++waited){
        if(capture_yielded_&&capture_ack_.load()==request)return true;
        usleep(10000);
    }
    exclusive_capture_=false;return false;
}
void AudioService::EndExclusiveCapture(){exclusive_capture_=false;}
bool AudioService::LocalPlaybackSettled() const {
    std::lock_guard<std::mutex>lock(audio_queue_mutex_);
    return !local_session_&&!output_local_session_&&AudioNowMs()>=local_output_tail_until_ms_;
}
bool AudioService::VoicePlaybackSettled() const {
    std::lock_guard<std::mutex>lock(audio_queue_mutex_);
    return !local_session_ && !output_in_progress_ && !decode_in_progress_ &&
        audio_playback_queue_.empty() && audio_decode_queue_.empty() &&
        std::chrono::steady_clock::now()-last_output_time_>=std::chrono::milliseconds(500);
}

bool AudioService::TryEnqueueLocalPcm(uint32_t session,const int16_t *data,size_t samples)
{
    if(!codec_||!data||!samples||!session)return false;
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if(session!=local_session_||audio_playback_queue_.size()>=MAX_PLAYBACK_TASKS_IN_QUEUE)return false;
    auto task=std::make_unique<AudioTask>();task->type=kAudioTaskTypeDecodeToPlaybackQueue;
    task->pcm.assign(data,data+samples);task->timestamp=0;task->local_session=session;
    // This NuttX condvar implementation drains wait_count during broadcast.
    // Keep its associated mutex held: a higher priority consumer could
    // otherwise re-enter wait before broadcast finishes and keep it spinning.
    audio_playback_queue_.push_back(std::move(task));audio_queue_cv_.notify_all();return true;
}

void AudioService::FlushLocalPlayback(uint32_t session)
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);if(!session||session!=local_session_)return;
    for(auto i=audio_playback_queue_.begin();i!=audio_playback_queue_.end();)
        if((*i)->local_session==session)i=audio_playback_queue_.erase(i);else ++i;
    local_buffering_=true;local_finished_=false;audio_queue_cv_.notify_all();
}
void AudioService::FinishLocalPlayback(uint32_t session)
{std::lock_guard<std::mutex> lock(audio_queue_mutex_);if(session&&session==local_session_)local_finished_=true;audio_queue_cv_.notify_all();}
bool AudioService::LocalPlaybackDrained(uint32_t session) const
{
    if(!session)return true;
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);if(output_local_session_==session)return false;
    for(const auto &q:audio_playback_queue_)if(q->local_session==session)return false;return true;
}
bool AudioService::LocalPlaybackWriteFailed(uint32_t session) const
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return !session || session!=local_session_ || local_write_failed_;
}
void AudioService::EndLocalPlayback(uint32_t session)
{
    if(!session)return;
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    for(auto i=audio_playback_queue_.begin();i!=audio_playback_queue_.end();)
        if((*i)->local_session==session)i=audio_playback_queue_.erase(i);else ++i;
    if(session==local_session_){local_session_=0;local_buffering_=false;local_finished_=false;}
    audio_queue_cv_.notify_all();
}
bool AudioService::HasLocalPlaybackSession() const
{std::lock_guard<std::mutex> lock(audio_queue_mutex_);return local_session_!=0;}

uint32_t AudioService::LocalPlaybackTimeMs(uint32_t session) const
{
 std::lock_guard<std::mutex> lock(audio_queue_mutex_);
 if(!session||session!=local_session_||!local_clock_start_ms_)return 0;
 const int64_t now=local_pause_ms_?local_pause_ms_:AudioNowMs();
 return (uint32_t)std::min<int64_t>(local_clock_samples_/16,std::max<int64_t>(0,now-local_clock_start_ms_));
}
void AudioService::PauseLocalPlayback(uint32_t session,bool pause)
{
 std::lock_guard<std::mutex> lock(audio_queue_mutex_);if(!session||session!=local_session_)return;
 if(pause&&!local_pause_ms_)local_pause_ms_=AudioNowMs();
 else if(!pause&&local_pause_ms_){if(local_clock_start_ms_)local_clock_start_ms_+=AudioNowMs()-local_pause_ms_;local_pause_ms_=0;}
 audio_queue_cv_.notify_all();
}

void AudioService::ClearWakeWordMute()
{
    wake_mute_until_ms_.store(0, std::memory_order_relaxed);
}

bool AudioService::IsIdle()
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return !decode_in_progress_ && !output_in_progress_ &&
           audio_encode_queue_.empty() && audio_decode_queue_.empty() &&
           audio_playback_queue_.empty() &&
           audio_testing_queue_.empty();
}

void AudioService::ResetDecoder()
{
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    // Decode runs outside the queue lock. Reset must wait for that worker
    // before touching the same Opus state or its replacement instance.
    audio_queue_cv_.wait(lock, [this] { return !decode_in_progress_; });
    if (opus_decoder_)
        opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    for(auto i=audio_playback_queue_.begin();i!=audio_playback_queue_.end();)
        if(!local_session_||(*i)->local_session!=local_session_)i=audio_playback_queue_.erase(i);else ++i;
    audio_testing_queue_.clear();
    wake_greeting_active_ = false;
    wake_greeting_started_ = false;
    wake_greeting_ended_ = false;
    wake_greeting_first_packet_ = {};
    wake_greeting_drain_until_ = {};
    audio_queue_cv_.notify_all();
}

void AudioService::ResetDecoderStateOnly()
{
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    // Decode runs outside the queue lock. Reset must wait for that worker
    // before touching the same Opus state or its replacement instance.
    audio_queue_cv_.wait(lock, [this] { return !decode_in_progress_; });
    if (opus_decoder_)
        opus_decoder_->ResetState();
}

bool AudioService::HasPendingPlayback() const
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return decode_in_progress_ || output_in_progress_ ||
           !audio_decode_queue_.empty() || !audio_playback_queue_.empty() ||
           (wake_greeting_active_ &&
            std::chrono::steady_clock::now() < wake_greeting_drain_until_);
}

void AudioService::BeginWakeGreetingPlayback()
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    wake_greeting_active_ = true;
    wake_greeting_started_ = false;
    wake_greeting_ended_ = false;
    wake_greeting_first_packet_ = {};
    wake_greeting_drain_until_ = {};
}

void AudioService::NotifyTtsStreamEnded()
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (wake_greeting_active_)
    {
        wake_greeting_ended_ = true;
        wake_greeting_drain_until_ = std::max(wake_greeting_drain_until_,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(300));
        audio_queue_cv_.notify_all();
    }
}

void AudioService::DiscardUplinkAudio()
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_send_queue_.clear();
    audio_queue_cv_.notify_all();
    write(1, "UL_DROP\n", 8);
}

bool AudioService::ShouldSuppressUplink() const
{
    /* AutoStop: mute the mic path only while TTS is gated. A leftover
     * playback buffer must not keep the next utterance from being sent
     * after the gate drops (that stuck the header on「聆听中」). */
    return tts_uplink_gate_.load(std::memory_order_relaxed);
}

bool AudioService::ShouldSuppressWakeWord() const
{
    if (metalio_nt26_get_call_state() != METALIO_CALL_IDLE)
        return true;
    if (protect_downlink_playback_.load(std::memory_order_relaxed))
        return true;
    return AudioNowMs() <
           wake_mute_until_ms_.load(std::memory_order_relaxed);
}

void AudioService::MuteWakeWordForMs(uint32_t ms)
{
    if (ms == 0)
        return;
    const uint64_t now=AudioNowMs();
    const bool already_muted=protect_downlink_playback_.load()||now<wake_mute_until_ms_.load();
    const uint64_t until = now + static_cast<uint64_t>(ms);
    uint64_t prev = wake_mute_until_ms_.load(std::memory_order_relaxed);
    while (until > prev &&
           !wake_mute_until_ms_.compare_exchange_weak(
               prev, until, std::memory_order_relaxed)) {
    }
    if(!already_muted){std::lock_guard<std::mutex> lock(wake_word_mutex_);
    if (wake_word_)wake_word_->Reset();}
}

void AudioService::SetTtsUplinkGate(bool gated)
{
    tts_uplink_gate_.store(gated, std::memory_order_release);
    if (gated)
    {
        barge_loud_frames_ = 0;
        barge_echo_floor_ = 400.0f;
        /* Ignore mic for ~0.45s so TTS onset / echo cannot false-barge. */
        barge_armed_after_ = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(450);
        barge_last_poll_ = {};
    }
    write(1, gated ? "UL_GATE_ON\n" : "UL_GATE_OFF\n",
          gated ? 11 : 12);
}

void AudioService::ArmDownlinkPlaybackProtect()
{
    const bool already_protected =
        protect_downlink_playback_.exchange(true, std::memory_order_acq_rel);
    /* Do NOT EnableOutput here — priming TX with silence while the mic is
     * live couples into the uplink (xiaozhi.me hears 电流杂音). Output opens
     * lazily on the first real PCM in AudioOutputTask. */
    if (!already_protected) write(1, "DL_ARM\n", 7);
}

void AudioService::ClearDownlinkPlaybackProtect()
{
    if (protect_downlink_playback_.exchange(false, std::memory_order_acq_rel))
        write(1, "DL_CLR\n", 7);
    /* Do NOT age last_output_time_ here. Forcing idle≃1000ms removed the
     * post-TTS mic holdoff and let speaker echo into STT (wrong text).
     * AudioOutputTask drops TX after gate-off idle ≥120ms. */
}

/* ------------------------------------------------------------------ */
/* Volume / mute                                                      */
/* ------------------------------------------------------------------ */

void AudioService::SetVolume(int volume)
{
    SetVolume(volume, true);
}

void AudioService::SetVolume(int volume, bool persist)
{
    if (codec_)
        codec_->SetOutputVolume(volume, persist);
}

int AudioService::GetVolume() const
{
    if (codec_)
        return codec_->output_volume();
    return 0;
}

void AudioService::SetMute(bool mute)
{
    if (codec_)
        codec_->SetMute(mute);
}

bool AudioService::IsMuted() const
{
    if (codec_)
        return codec_->muted();
    return false;
}

/* ------------------------------------------------------------------ */
/* Press-to-talk (PTT) detection                                      */
/* ------------------------------------------------------------------ */

bool AudioService::IsPttPressed() const
{
    /*
     * PTT button detection.
     *
     * On the MetalioClaw-4 board, the BOOT button (GPIO) is used as the
     * PTT trigger.  The NuttX board support package exposes this via a
     * GPIO character device or a board-specific helper.
     *
     * For now, this reads the BOOT button via the standard NuttX GPIO
     * ioctl interface.  When the board BSP provides a dedicated PTT
     * helper, replace this implementation.
     *
     * TODO: Replace with board-specific button driver access.
     */
    return false;
}

/* ------------------------------------------------------------------ */
/* Power management                                                   */
/* ------------------------------------------------------------------ */

void AudioService::CheckAndUpdateAudioPowerState()
{
    /* Shared I2S0: do not EnableInput/EnableOutput(false). That flag used
     * to pair with AUDIOIOC_STOP and the driver stops the other direction
     * when a queue drains. Wake, listen, and TTS leave both DMA channels
     * armed; AutoStop only gates the uplink. */
    if (audio_power_timer_)
        esp_timer_stop(audio_power_timer_);
}

uint32_t AudioService::BeginNotification(){std::lock_guard<std::mutex>l(audio_queue_mutex_);notification_pcm_.clear();return ++notification_epoch_;}
bool AudioService::EnqueueNotification(uint32_t id,const int16_t*data,size_t count){std::lock_guard<std::mutex>l(audio_queue_mutex_);if(!id||id!=notification_epoch_||exclusive_capture_||(local_session_&&local_complete_write_)||count>16000||notification_pcm_.size()+count>16000)return false;notification_pcm_.insert(notification_pcm_.end(),data,data+count);audio_queue_cv_.notify_all();return true;}
void AudioService::EndNotification(uint32_t id){std::lock_guard<std::mutex>l(audio_queue_mutex_);if(id==notification_epoch_){++notification_epoch_;notification_pcm_.clear();}}
bool AudioService::NotificationDrained(uint32_t id)const{std::lock_guard<std::mutex>l(audio_queue_mutex_);return id!=notification_epoch_||(notification_pcm_.empty()&&!output_in_progress_);}
