/*
 * AfeWakeWord — ESP-SR AFE + WakeNet port for NuttX Metalio.
 * Based on MetalioClaw4 main/audio/wake_words/afe_wake_word.cc.
 */

#include "afe_wake_word.h"
#include "opus_codec.h"
#include "esp_log_shim.h"
#include "esp_heap_caps.h"
#include "esp_timer_shim.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <unistd.h>

#define DETECTION_RUNNING_EVENT (1 << 0)
#define DETECTION_TASK_EXIT     (1 << 1)

#define TAG "AfeWakeWord"

AfeWakeWord::AfeWakeWord()
{
    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord()
{
    Deinitialize();

    if (detect_task_ != 0)
    {
        xEventGroupSetBits(event_group_, DETECTION_TASK_EXIT);
        for (int i = 0; i < 50 && detect_task_ != 0; ++i)
            vTaskDelay(pdMS_TO_TICKS(20));
        detect_task_ = 0;
    }

    if (models_owned_ && models_ != nullptr)
    {
        esp_srmodel_deinit(models_);
        models_ = nullptr;
    }

    if (event_group_)
        vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec *codec, srmodel_list_t *models_list)
{
    write(1, "WW_IA\n", 6);
    write(1, "WW_IB\n", 6);
    if (afe_data_ != nullptr)
    {
        write(1, "WW_SKIP\n", 8);
        return true;
    }

    if (codec == nullptr)
    {
        write(1, "WW_NCOD\n", 8);
        return false;
    }
    codec_ = codec;
    write(1, "WW_IC0\n", 7);
    int ref_num = codec_->input_reference() ? 1 : 0;
    write(1, "WW_IC1\n", 7);

    if (models_ == nullptr)
    {
        if (models_list == nullptr)
        {
            ESP_LOGE(TAG, "No srmodels list");
            write(1, "WW_NOM\n", 7);
            return false;
        }
        models_ = models_list;
        models_owned_ = false;
    }

    if (models_ == nullptr || models_->num <= 0)
    {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        write(1, "WW_NOM2\n", 8);
        return false;
    }

    write(1, "WW_IC\n", 6);
    if (wake_words_.empty())
    {
        for (int i = 0; i < models_->num; i++)
        {
            ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
            if (strstr(models_->model_name[i], ESP_WN_PREFIX) != nullptr)
            {
                wakenet_model_ = models_->model_name[i];
                write(1, "WW_ID\n", 6);
                auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
                write(1, "WW_IE\n", 6);
                if (words)
                {
                    /* Avoid stringstream on uClibc++ — parse with strtok. */
                    char *save = nullptr;
                    for (char *tok = strtok_r(words, ";", &save); tok != nullptr;
                         tok = strtok_r(nullptr, ";", &save))
                        wake_words_.push_back(tok);
                    free(words);
                }
            }
        }
    }
    if (wake_words_.empty())
        wake_words_.push_back("Hi openvela");

    write(1, "WW_IF\n", 6);
    /* Bring-up: mic-only. AEC/MR for wake can race HomeScreen heap; barge-in
     * AEC lives in AfeAudioProcessor. (ref_num kept for logs only.) */
    (void)ref_num;
    const char *input_format = "M";
    char fmt_dbg[16];
    snprintf(fmt_dbg, sizeof(fmt_dbg), "WW_FMT_%s\n", input_format);
    write(1, fmt_dbg, strlen(fmt_dbg));

    write(1, "WW_I0\n", 6);

    /* Build AFE config manually — afe_config_init() has hung indefinitely on
     * this NuttX port (likely printf dump / model walk). */
    afe_config_t *afe_config = afe_config_alloc();
    if (afe_config == nullptr)
    {
        write(1, "WW_I0F\n", 7);
        return false;
    }
    write(1, "WW_I0a\n", 7);

    if (!afe_parse_input_format(input_format, &afe_config->pcm_config))
    {
        write(1, "WW_I0P\n", 7);
        afe_config_free(afe_config);
        return false;
    }
    write(1, "WW_I0p\n", 7);

    afe_config->afe_type = AFE_TYPE_SR;
    afe_config->afe_mode = AFE_MODE_LOW_COST;
    afe_config->aec_init = false;
    afe_config->aec_mode = AEC_MODE_SR_LOW_COST;
    afe_config->aec_filter_length = 4;
    afe_config->se_init = false;
    afe_config->ns_init = false;
    afe_config->ns_model_name = nullptr;
    afe_config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    afe_config->vad_init = false;
    afe_config->vad_mode = VAD_MODE_3;
    afe_config->vad_model_name = nullptr;
    afe_config->vad_min_speech_ms = 128;
    afe_config->vad_min_noise_ms = 1000;
    afe_config->vad_delay_ms = 128;
    afe_config->vad_mute_playback = false;
    afe_config->vad_enable_channel_trigger = false;
    write(1, "WW_I0q\n", 7);
    afe_config->wakenet_init = true;
    afe_config->wakenet_model_name = wakenet_model_;
    afe_config->wakenet_model_name_2 = nullptr;
    afe_config->wakenet_mode = DET_MODE_90;
    afe_config->agc_init = false;
    afe_config->agc_mode = AFE_AGC_MODE_WAKENET;
    afe_config->agc_compression_gain_db = 9;
    afe_config->agc_target_level_dbfs = 3;
    afe_config->afe_perferred_core = 0;
    afe_config->afe_perferred_priority = 5;
    afe_config->afe_ringbuf_size = 12;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_INTERNAL;
    afe_config->afe_linear_gain = 1.0f;
    afe_config->debug_init = false;
    afe_config->fixed_first_channel = true;
    write(1, "WW_I0b\n", 7);
    usleep(20000);
    write(1, "WW_I1\n", 6);
    usleep(20000);
    write(1, "WW_I1c\n", 7);
    usleep(20000);

    write(1, "WW_I2\n", 6);
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    write(1, "WW_I2b\n", 7);
    if (afe_iface_ == nullptr)
    {
        write(1, "WW_I2F\n", 7);
        afe_config_free(afe_config);
        return false;
    }
    write(1, "WW_I3\n", 6);

    /* create_from_config has hung past WW_I3; run with timeout + heartbeat. */
    struct CreateJob
    {
        const esp_afe_sr_iface_t *iface;
        afe_config_t *cfg;
        esp_afe_sr_data_t *out;
        volatile int done;
    } job = {afe_iface_, afe_config, nullptr, 0};

    pthread_t th = 0;
    int pret = -1;
    if (afe_iface_)
    {
        pthread_attr_t cattr;
        pthread_attr_init(&cattr);
        pthread_attr_setstacksize(&cattr, 256 * 1024);
        pret = pthread_create(
            &th, &cattr,
            [](void *arg) -> void * {
                auto *j = static_cast<CreateJob *>(arg);
                write(1, "WW_I3a\n", 7);
                j->out = j->iface->create_from_config(j->cfg);
                write(1, "WW_I3b\n", 7);
                j->done = 1;
                return nullptr;
            },
            &job);
        pthread_attr_destroy(&cattr);
    }
    if (pret != 0)
    {
        write(1, "WW_I3P\n", 7);
        afe_config_free(afe_config);
        afe_iface_ = nullptr;
        return false;
    }
    for (int i = 0; i < 120 && !job.done; ++i) /* 12s */
    {
        usleep(100000);
        if ((i % 10) == 9)
            write(1, "WW_I3H\n", 7);
    }
    if (!job.done)
    {
        write(1, "WW_I3T\n", 7);
        ESP_LOGE(TAG, "create_from_config timed out");
        /* Leave worker detached; do not free config while it may still run. */
        pthread_detach(th);
        afe_iface_ = nullptr;
        return false;
    }
    pthread_join(th, nullptr);
    afe_data_ = job.out;
    write(1, "WW_I4\n", 6);
    afe_config_free(afe_config);
    if (afe_data_ == nullptr)
    {
        ESP_LOGE(TAG, "create_from_config failed");
        write(1, "WW_I4F\n", 7);
        afe_iface_ = nullptr;
        return false;
    }

    write(1, "WW_I5\n", 6);
    if (detect_task_ == 0)
    {
        xTaskCreate(
            [](void *arg) {
                auto *self = static_cast<AfeWakeWord *>(arg);
                self->AudioDetectionTask();
                self->detect_task_ = 0;
                vTaskDelete(nullptr);
            },
            "audio_detection", 8192, this, 3, &detect_task_);
    }
    write(1, "WW_I6\n", 6);

    write(1, "WW_AFE_OK\n", 10);
    ESP_LOGI(TAG, "Wake word AFE created format=%s wn=%s", input_format,
             wakenet_model_ ? wakenet_model_ : "?");
    return true;
}

void AfeWakeWord::Deinitialize()
{
    Stop();

    for (int i = 0; i < 40 && fetching_.load(std::memory_order_acquire); ++i)
        vTaskDelay(pdMS_TO_TICKS(5));

    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr)
        return;

    ESP_LOGI(TAG, "Destroying wake word AFE");
    afe_iface_->destroy(afe_data_);
    afe_data_ = nullptr;
}

void AfeWakeWord::OnWakeWordDetected(
    std::function<void(const std::string &wake_word)> callback)
{
    wake_word_detected_callback_ = std::move(callback);
}

void AfeWakeWord::Start()
{
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr)
    {
        ESP_LOGW(TAG, "Start ignored: AFE not initialized");
        return;
    }
    afe_iface_->enable_wakenet(afe_data_);
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop()
{
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ != nullptr && afe_iface_ != nullptr)
        afe_iface_->disable_wakenet(afe_data_);
}

void AfeWakeWord::Feed(const std::vector<int16_t> &data)
{
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr)
        return;
    afe_iface_->feed(afe_data_, data.data());
}

size_t AfeWakeWord::GetFeedSize()
{
    std::lock_guard<std::mutex> lock(afe_mutex_);
    if (afe_data_ == nullptr || afe_iface_ == nullptr)
        return 0;
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask()
{
    ESP_LOGI(TAG, "Audio detection task started");

    for (;;)
    {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, DETECTION_RUNNING_EVENT | DETECTION_TASK_EXIT, pdFALSE,
            pdFALSE, portMAX_DELAY);

        if (bits & DETECTION_TASK_EXIT)
            break;

        const esp_afe_sr_iface_t *iface = nullptr;
        esp_afe_sr_data_t *data = nullptr;
        {
            std::lock_guard<std::mutex> lock(afe_mutex_);
            if (afe_data_ == nullptr || afe_iface_ == nullptr)
            {
                xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
                continue;
            }
            fetching_.store(true, std::memory_order_release);
            iface = afe_iface_;
            data = afe_data_;
        }

        auto res = iface->fetch_with_delay(data, pdMS_TO_TICKS(50));
        fetching_.store(false, std::memory_order_release);

        if ((xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT) == 0)
            continue;
        {
            std::lock_guard<std::mutex> lock(afe_mutex_);
            if (afe_data_ != data)
                continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL)
            continue;

        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        if (res->wakeup_state == WAKENET_DETECTED)
        {
            Stop();
            const int idx = res->wakenet_model_index - 1;
            if (idx >= 0 && (size_t)idx < wake_words_.size())
                last_detected_wake_word_ = wake_words_[(size_t)idx];
            else
                last_detected_wake_word_ = "Hi openvela";
            write(1, "WW_HIT\n", 7);
            if (wake_word_detected_callback_)
                wake_word_detected_callback_(last_detected_wake_word_);
        }
    }

    ESP_LOGI(TAG, "Audio detection task exit");
}

void AfeWakeWord::StoreWakeWordData(const int16_t *data, size_t samples)
{
    wake_word_pcm_.push_back(std::vector<int16_t>(data, data + samples));
    while (wake_word_pcm_.size() > 2000 / 30)
        wake_word_pcm_.pop_front();
}

void AfeWakeWord::EncodeWakeWordData()
{
    if (encode_running_.exchange(true))
        return;

    wake_word_opus_.clear();
    std::thread([this]() {
        auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, 60);
        encoder->SetComplexity(0);
        int packets = 0;
        for (auto &pcm : wake_word_pcm_)
        {
            std::vector<uint8_t> opus;
            if (encoder->Encode(std::move(pcm), opus) && !opus.empty())
            {
                std::lock_guard<std::mutex> lock(wake_word_mutex_);
                wake_word_opus_.push_back(std::move(opus));
                wake_word_cv_.notify_all();
                packets++;
            }
        }
        wake_word_pcm_.clear();
        {
            std::lock_guard<std::mutex> lock(wake_word_mutex_);
            wake_word_opus_.push_back(std::vector<uint8_t>()); /* empty = end */
            wake_word_cv_.notify_all();
        }
        ESP_LOGI(TAG, "Encode wake word opus %d packets", packets);
        encode_running_.store(false);
    }).detach();
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t> &opus)
{
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
