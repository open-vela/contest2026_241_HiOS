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
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "board_shim.h"
#include "display.h"

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
#include <cmath>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <pthread.h>

#define TAG "AudioService"

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

    /* Prefer AfeWakeWord when models are present; EnergyWakeWord as fallback. */
    wake_word_ = std::make_unique<EnergyWakeWord>();
    wake_word_->OnWakeWordDetected(
        [this](const std::string& wake_word)
        {
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
    audio_input_thread_ = std::thread([this]() { AudioInputTask(); });
    write(1, "AS3\n", 4);
    audio_output_thread_ = std::thread([this]() { AudioOutputTask(); });
    write(1, "AS4\n", 4);
    opus_codec_thread_   = std::thread([this]() { OpusCodecTask(); });
    write(1, "AS5\n", 4);

    /* Do not ESP_LOGI / spawn WakeNet here — that path blue-screened boot.
     * EnergyWakeWord stays default; see SetModelsList / EnsureWakeWordReady. */
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
    }
    audio_queue_cv_.notify_all();

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
    events_.fetch_or(AS_EVENT_INPUT_TASK_RUNNING);
    ESP_LOGI(TAG, "Audio input task started");

    while (true)
    {
        uint32_t bits = events_.load();
        /* Check if any of the "feed" modes are active */
        uint32_t feed_bits = bits & (AS_EVENT_AUDIO_TESTING_RUNNING |
                                     AS_EVENT_WAKE_WORD_RUNNING |
                                     AS_EVENT_AUDIO_PROCESSOR_RUNNING);

        if (service_stopped_)
            break;

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
    if (!device_aec_enabled_ || !callbacks_.on_barge_in)
        return;
    if (!tts_uplink_gate_.load(std::memory_order_relaxed))
        return;
    /* Allow barge even if playback briefly emptied — waiting for
     * HasPendingPlayback() missed many soft interrupts. */

    const auto now = std::chrono::steady_clock::now();
    if (now < barge_armed_after_ || now < barge_cooldown_until_)
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

    /* Learn TTS bleed floor; barge on near-field speech over speaker. */
    if (rms < barge_echo_floor_ * 1.5f)
        barge_echo_floor_ = barge_echo_floor_ * 0.9f + rms * 0.1f;
    const float thr = std::max(barge_echo_floor_ * 2.8f, 900.0f);
    if (rms >= thr && peak >= 4500)
        barge_loud_frames_++;
    else
        barge_loud_frames_ = 0;

    /* One sustained loud peek (~220ms) is enough to interrupt. */
    if (barge_loud_frames_ >= 1)
    {
        barge_loud_frames_ = 0;
        barge_cooldown_until_ = now + std::chrono::milliseconds(800);
        write(1, "BARGE\n", 6);
        callbacks_.on_barge_in();
    }
}

/* ------------------------------------------------------------------ */
/* Audio output task (speaker playback)                               */
/* ------------------------------------------------------------------ */

void AudioService::AudioOutputTask()
{
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
        if (service_stopped_)
            break;

        if (!audio_playback_queue_.empty())
        {
            auto task = std::move(audio_playback_queue_.front());
            audio_playback_queue_.pop_front();
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
            codec_->OutputData(task->pcm);
            last_output_time_ = std::chrono::steady_clock::now();
            debug_statistics_.playback_count++;
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
        if (speaking_tx && codec_ && codec_->output_enabled() &&
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

        /* Never TX_OFF while Speaking — idle≥80 with gate still on was
         * killing mid-utterance playback (silent feedback + stutter).
         * After gate clears, wait briefly so the last PCM can drain. */
        const bool protect =
            protect_downlink_playback_.load(std::memory_order_relaxed);
        if (codec_ && codec_->output_enabled() && !protect && !speaking_tx &&
            idle_ms >= 120)
        {
            lock.unlock();
            codec_->EnableOutput(false);
            continue;
        }

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

        /* Decode — match Claw4: unlock while Decode/resample so AudioOutputTask
         * can keep feeding I2S (holding the lock here caused TX underrun 杂音). */
        if (!audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE)
        {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
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
                audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
            }
            else
            {
                ESP_LOGE(TAG, "Failed to decode audio");
                lock.lock();
            }
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
            return;

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
        audio_input_need_warmup_ = true;
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
    /* Keep EnergyWakeWord only. Never kick Esp/WakeNet from boot/models
     * load — that raced DISP start (WW_ESP_TRY during DISP1) and left
     * chat/standby solid blue. Application upgrades after chat settles. */
    write(1, "WW_ENERGY\n", 10);
}

void AudioService::EnsureWakeWordReady()
{
    /* Never create EspWakeWord/WakeNet on this port — EW_CR1 during chat
     * (first wake especially) solid-blues LVGL. EnergyWakeWord stays. */
    write(1, "WW_SKIP_ESP\n", 12);
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

bool AudioService::IsIdle()
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() &&
           audio_decode_queue_.empty() &&
           audio_playback_queue_.empty() &&
           audio_testing_queue_.empty();
}

void AudioService::ResetDecoder()
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (opus_decoder_)
        opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::ResetDecoderStateOnly()
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (opus_decoder_)
        opus_decoder_->ResetState();
}

bool AudioService::HasPendingPlayback() const
{
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return !audio_decode_queue_.empty() || !audio_playback_queue_.empty();
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
    /* Always mute uplink while TTS is gated or PCM is queued. Opening the
     * mic during TTS (even with "device AEC") leaked speaker into STT on
     * this port → 自问自答 / chopped feedback. Interrupt uses local barge. */
    if (tts_uplink_gate_.load(std::memory_order_relaxed))
        return true;
    if (HasPendingPlayback())
        return true;
    return false;
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
    protect_downlink_playback_.store(true, std::memory_order_release);
    /* Do NOT EnableOutput here — priming TX with silence while the mic is
     * live couples into the uplink (xiaozhi.me hears 电流杂音). Output opens
     * lazily on the first real PCM in AudioOutputTask. */
    write(1, "DL_ARM\n", 7);
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
    if (codec_)
        codec_->SetOutputVolume(volume);
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
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_input_time_).count();
    auto output_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_output_time_).count();

    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled())
        codec_->EnableInput(false);

    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled())
        codec_->EnableOutput(false);

    if (!codec_->input_enabled() && !codec_->output_enabled())
    {
        if (audio_power_timer_)
            esp_timer_stop(audio_power_timer_);
    }
}
