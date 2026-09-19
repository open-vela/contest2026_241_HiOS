/*
 * EspWakeWord — WakeNet without AFE (MetalioClaw4 esp_wake_word.cc port).
 */

#include "esp_wake_word.h"
#include "esp_log_shim.h"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#define TAG "EspWakeWord"

EspWakeWord::EspWakeWord() = default;

EspWakeWord::~EspWakeWord()
{
    Deinitialize();
    if (models_owned_ && models_ != nullptr)
    {
        esp_srmodel_deinit(models_);
        models_ = nullptr;
    }
}

void EspWakeWord::Deinitialize()
{
    running_.store(false);
    if (wakenet_data_ != nullptr && wakenet_iface_ != nullptr)
    {
        wakenet_iface_->destroy(wakenet_data_);
        wakenet_data_ = nullptr;
    }
    wakenet_iface_ = nullptr;
}

bool EspWakeWord::Initialize(AudioCodec *codec, srmodel_list_t *models_list)
{
    write(1, "EW_IA\n", 6);
    codec_ = codec;
    if (wakenet_data_ != nullptr)
    {
        write(1, "EW_SKIP\n", 8);
        return true;
    }

    if (models_ == nullptr)
    {
        if (models_list == nullptr)
        {
            write(1, "EW_NOM\n", 7);
            return false;
        }
        models_ = models_list;
        models_owned_ = false;
    }

    if (models_ == nullptr || models_->num <= 0)
    {
        write(1, "EW_NOM2\n", 8);
        return false;
    }

    model_name_ = nullptr;
    for (int i = 0; i < models_->num; i++)
    {
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != nullptr)
        {
            model_name_ = models_->model_name[i];
            break;
        }
    }
    if (model_name_ == nullptr)
        model_name_ = models_->model_name[0];

    write(1, "EW_WN0\n", 7);
    wakenet_iface_ = (esp_wn_iface_t *)esp_wn_handle_from_name(model_name_);
    write(1, wakenet_iface_ ? "EW_WN1\n" : "EW_WNF\n", 7);
    if (wakenet_iface_ == nullptr)
        return false;

    write(1, "EW_CR1\n", 7);
    /* create() has hard-locked on some boots after SDIO storms; bound it. */
    {
        static esp_wn_iface_t *s_iface;
        static const char *s_name;
        static model_iface_data_t *s_data;
        static volatile bool s_done;

        s_iface = wakenet_iface_;
        s_name = model_name_;
        s_data = nullptr;
        s_done = false;

        pthread_attr_t attr;
        pthread_t th;
        pthread_attr_init(&attr);
        (void)pthread_attr_setstacksize(&attr, 192 * 1024);
        int prc = pthread_create(
            &th, &attr,
            [](void *) -> void * {
                s_data = s_iface->create(s_name, DET_MODE_95);
                s_done = true;
                return nullptr;
            },
            nullptr);
        pthread_attr_destroy(&attr);
        if (prc != 0)
        {
            write(1, "EW_CRP\n", 7);
            wakenet_iface_ = nullptr;
            return false;
        }

        for (int i = 0; i < 150 && !s_done; ++i)
            usleep(100000);

        if (!s_done)
        {
            write(1, "EW_CRT\n", 7);
            pthread_detach(th);
            wakenet_iface_ = nullptr;
            return false;
        }

        pthread_join(th, nullptr);
        wakenet_data_ = s_data;
    }
    write(1, wakenet_data_ ? "EW_CR2\n" : "EW_CRF\n", 7);
    if (wakenet_data_ == nullptr)
    {
        wakenet_iface_ = nullptr;
        return false;
    }

    write(1, "EW_OK\n", 6);
    return true;
}

void EspWakeWord::OnWakeWordDetected(
    std::function<void(const std::string &wake_word)> callback)
{
    wake_word_detected_callback_ = std::move(callback);
}

void EspWakeWord::Start()
{
    running_.store(true);
}

void EspWakeWord::Stop()
{
    running_.store(false);
}

void EspWakeWord::Feed(const std::vector<int16_t> &data)
{
    if (wakenet_data_ == nullptr || wakenet_iface_ == nullptr || !running_.load())
        return;

    /* Codec may deliver mic+ref interleaved; WakeNet wants mono mic. */
    const int16_t *mic = data.data();
    std::vector<int16_t> mono;
    int channels = 1;
    if (codec_ != nullptr)
    {
        int ref = codec_->input_reference() ? 1 : 0;
        channels = codec_->input_channels();
        if (channels > 1)
        {
            const size_t frames = data.size() / (size_t)channels;
            mono.resize(frames);
            for (size_t i = 0; i < frames; ++i)
                mono[i] = data[i * (size_t)channels];
            mic = mono.data();
            (void)ref;
        }
    }

    int res = wakenet_iface_->detect(wakenet_data_, const_cast<int16_t *>(mic));
    if (res > 0)
    {
        last_detected_wake_word_ =
            wakenet_iface_->get_word_name(wakenet_data_, res);
        if (last_detected_wake_word_.empty())
            last_detected_wake_word_ = "Hi openvela";
        running_.store(false);
        write(1, "WW_HIT\n", 7);
        if (wake_word_detected_callback_)
            wake_word_detected_callback_(last_detected_wake_word_);
    }
}

size_t EspWakeWord::GetFeedSize()
{
    if (wakenet_data_ == nullptr || wakenet_iface_ == nullptr)
        return 0;
    return wakenet_iface_->get_samp_chunksize(wakenet_data_);
}

void EspWakeWord::EncodeWakeWordData() {}

bool EspWakeWord::GetWakeWordOpus(std::vector<uint8_t> &opus)
{
    (void)opus;
    return false;
}
