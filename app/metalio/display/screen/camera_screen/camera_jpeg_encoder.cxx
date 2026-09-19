/*
 * BGR888 -> JPEG encoder for CameraScreen photo save.
 *
 * Uses stb_image_write (public domain) for baseline JPEG output.  Canvas /
 * preview buffers are B-G-R; STB expects R-G-B so we swap channels in a
 * scratch buffer before encoding.
 */

#include "camera_jpeg_encoder.h"

#include "esp_log_shim.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

constexpr const char *TAG = "CamJpeg";

struct MemWriter {
    std::vector<uint8_t> bytes;
};

void StbWriteThunk(void *context, void *data, int size)
{
    auto *writer = static_cast<MemWriter *>(context);
    const auto *src = static_cast<const uint8_t *>(data);
    writer->bytes.insert(writer->bytes.end(), src, src + size);
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

        std::vector<uint8_t> rgb(need);
        for (size_t i = 0; i < need; i += 3) {
            rgb[i + 0] = bgr[i + 2];
            rgb[i + 1] = bgr[i + 1];
            rgb[i + 2] = bgr[i + 0];
        }

        MemWriter writer;
        const int ok = stbi_write_jpg_to_func(
            StbWriteThunk, &writer, w, h, 3, rgb.data(), quality);
        if (ok == 0 || writer.bytes.empty()) {
            ESP_LOGE(TAG, "stbi_write_jpg_to_func failed (%dx%d q=%d)", w, h,
                     quality);
            return false;
        }

        uint8_t *jpeg = static_cast<uint8_t *>(std::malloc(writer.bytes.size()));
        if (jpeg == nullptr) {
            ESP_LOGE(TAG, "malloc(%u) failed",
                     static_cast<unsigned>(writer.bytes.size()));
            return false;
        }

        std::memcpy(jpeg, writer.bytes.data(), writer.bytes.size());
        *out     = jpeg;
        *out_len = writer.bytes.size();
        ESP_LOGI(TAG, "encoded %dx%d -> %u bytes JPEG (q=%d)", w, h,
                 static_cast<unsigned>(*out_len), quality);
        return true;
    }

private:
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
