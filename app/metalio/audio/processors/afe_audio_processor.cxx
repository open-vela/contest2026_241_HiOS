/*
 * AfeAudioProcessor — device AEC / NS / VAD for Realtime barge-in.
 * Based on MetalioClaw4 main/audio/processors/afe_audio_processor.cc.
 */

#include "afe_audio_processor.h"
#include "esp_log_shim.h"

#include <unistd.h>

#define PROCESSOR_RUNNING 0x01
#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
{
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec *codec, int frame_duration_ms,
                                   srmodel_list_t *models_list)
{
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++)
        input_format.push_back('M');
    for (int i = 0; i < ref_num; i++)
        input_format.push_back('R');

    srmodel_list_t *models = models_list;
    char *ns_model_name =
        models ? esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL) : nullptr;
    char *vad_model_name =
        models ? esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL) : nullptr;

    afe_config_t *afe_config =
        afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (afe_config == nullptr)
    {
        ESP_LOGE(TAG, "afe_config_init failed");
        return;
    }
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr)
        afe_config->vad_model_name = vad_model_name;

    if (ns_model_name != nullptr)
    {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    }
    else
    {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_INTERNAL;
    afe_config->aec_init = true;
    afe_config->vad_init = false;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_ ? afe_iface_->create_from_config(afe_config) : nullptr;
    afe_config_free(afe_config);

    if (afe_data_ == nullptr)
    {
        ESP_LOGE(TAG, "create_from_config failed");
        afe_iface_ = nullptr;
        return;
    }

    if (task_ == 0)
    {
        xTaskCreate(
            [](void *arg) {
                auto *self = static_cast<AfeAudioProcessor *>(arg);
                self->AudioProcessorTask();
                vTaskDelete(nullptr);
            },
            "audio_communication", 4096, this, 3, &task_);
    }

    write(1, "AFE_PROC_OK\n", 12);
    ESP_LOGI(TAG, "Voice AFE created format=%s", input_format.c_str());
}

AfeAudioProcessor::~AfeAudioProcessor()
{
    Stop();
    if (afe_data_ != nullptr && afe_iface_ != nullptr)
        afe_iface_->destroy(afe_data_);
    if (event_group_)
        vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize()
{
    if (afe_data_ == nullptr || afe_iface_ == nullptr)
        return 0;
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t> &&data)
{
    if (afe_data_ == nullptr || afe_iface_ == nullptr)
        return;
    afe_iface_->feed(afe_data_, data.data());
}

void AfeAudioProcessor::Start()
{
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop()
{
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr && afe_iface_ != nullptr)
        afe_iface_->reset_buffer(afe_data_);
}

bool AfeAudioProcessor::IsRunning()
{
    return (xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) != 0;
}

void AfeAudioProcessor::OnOutput(
    std::function<void(std::vector<int16_t> &&data)> callback)
{
    output_callback_ = std::move(callback);
}

void AfeAudioProcessor::OnVadStateChange(
    std::function<void(bool speaking)> callback)
{
    vad_state_change_callback_ = std::move(callback);
}

void AfeAudioProcessor::AudioProcessorTask()
{
    if (afe_iface_ == nullptr || afe_data_ == nullptr)
        return;

    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed=%d fetch=%d",
             feed_size, fetch_size);

    for (;;)
    {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE,
                            portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0)
            continue;
        if (res == nullptr || res->ret_value == ESP_FAIL)
            continue;

        if (vad_state_change_callback_)
        {
            if (res->vad_state == VAD_SPEECH && !is_speaking_)
            {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            }
            else if (res->vad_state == VAD_SILENCE && is_speaking_)
            {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (!output_callback_)
            continue;

        size_t samples = res->data_size / sizeof(int16_t);
        output_buffer_.insert(output_buffer_.end(), res->data,
                              res->data + samples);
        while (output_buffer_.size() >= (size_t)frame_samples_)
        {
            if (output_buffer_.size() == (size_t)frame_samples_)
            {
                output_callback_(std::move(output_buffer_));
                output_buffer_.clear();
                output_buffer_.reserve(frame_samples_);
            }
            else
            {
                output_callback_(std::vector<int16_t>(
                    output_buffer_.begin(),
                    output_buffer_.begin() + frame_samples_));
                output_buffer_.erase(output_buffer_.begin(),
                                     output_buffer_.begin() + frame_samples_);
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable)
{
    if (afe_iface_ == nullptr || afe_data_ == nullptr)
    {
        ESP_LOGE(TAG, "EnableDeviceAec: AFE not initialized");
        return;
    }
    if (enable)
    {
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
        write(1, "AEC_ON\n", 7);
    }
    else
    {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
        write(1, "AEC_OFF\n", 8);
    }
}
