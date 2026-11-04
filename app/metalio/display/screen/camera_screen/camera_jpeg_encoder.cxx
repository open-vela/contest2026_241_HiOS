/*
 * BGR888 -> JPEG encoder for CameraScreen photo save.
 *
 * Uses stb_image_write (public domain) for baseline JPEG output.  Canvas /
 * preview buffers are B-G-R; local STB channel access reads that order
 * directly, avoiding a full-frame RGB scratch allocation.
 */

#include "camera_jpeg_encoder.h"

#include "esp_log_shim.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#define STBI_WRITE_NO_STDIO
#define STBIW_JPEG_INPUT_BGR
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

constexpr const char *TAG = "CamJpeg";

struct MemWriter {
    uint8_t *bytes=nullptr;
    size_t size=0,capacity=0,limit=0;
    bool failed=false;
    ~MemWriter(){std::free(bytes);}
};

void StbWriteThunk(void *context, void *data, int size)
{
    auto *writer = static_cast<MemWriter *>(context);
    const auto *src = static_cast<const uint8_t *>(data);
    if(writer->failed || size<=0)return;
    const size_t need=writer->size+static_cast<size_t>(size);
    if(need>writer->limit){writer->failed=true;return;}
    if(need>writer->capacity){
        size_t capacity=writer->capacity?writer->capacity:16384;
        while(capacity<need)capacity=capacity>writer->limit/2?writer->limit:capacity*2;
        auto *next=static_cast<uint8_t*>(std::realloc(writer->bytes,capacity));
        if(!next){writer->failed=true;return;}
        writer->bytes=next;writer->capacity=capacity;
    }
    std::memcpy(writer->bytes+writer->size,src,size);writer->size=need;
}

class StbJpegEncoder : public CameraScreen::JpegEncoder
{
public:
    static StbJpegEncoder &Instance()
    {
        static StbJpegEncoder inst;
        return inst;
    }

    bool Encode(const uint8_t *bgr, size_t bgr_size, int w, int h, int quality,
                uint8_t **out, size_t *out_len) override
    {
        if (out == nullptr || out_len == nullptr) {
            return false;
        }
        *out     = nullptr;
        *out_len = 0;

        // Hi Vision's archive, visual requests and manual shutter share one
        // bounded encoder. Concurrent multi-megabyte vector growth could
        // terminate the process on memory exhaustion instead of failing.
        std::lock_guard<std::mutex> lock(mutex_);

        if (bgr == nullptr || w <= 0 || h <= 0) {
            return false;
        }

        const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
        if (bgr_size < need) {
            ESP_LOGE(TAG, "buffer too small: need %u got %u",
                     static_cast<unsigned>(need), static_cast<unsigned>(bgr_size));
            return false;
        }

        if (quality < 1) {
            quality = 1;
        } else if (quality > 100) {
            quality = 100;
        }

        MemWriter writer;
        writer.limit=need*2+65536;
        const int ok = stbi_write_jpg_to_func(
            StbWriteThunk, &writer, w, h, 3, bgr, quality);
        if (ok == 0 || writer.failed || writer.size<4 ||
            writer.bytes[0]!=0xff || writer.bytes[1]!=0xd8 ||
            writer.bytes[writer.size-2]!=0xff || writer.bytes[writer.size-1]!=0xd9) {
            ESP_LOGE(TAG, "stbi_write_jpg_to_func failed (%dx%d q=%d)", w, h,
                     quality);
            return false;
        }

        *out=writer.bytes;*out_len=writer.size;writer.bytes=nullptr;
        ESP_LOGI(TAG, "encoded %dx%d -> %u bytes JPEG (q=%d)", w, h,
                 static_cast<unsigned>(*out_len), quality);
        return true;
    }

private:
    std::mutex mutex_;
    StbJpegEncoder() = default;
};

struct EncoderRegistrar {
    EncoderRegistrar()
    {
        CameraScreen::SetJpegEncoder(&StbJpegEncoder::Instance());
    }
};

static EncoderRegistrar s_encoder_reg;

}  // namespace

CameraScreen::JpegEncoder *GetCameraJpegEncoder()
{
    return &StbJpegEncoder::Instance();
}
