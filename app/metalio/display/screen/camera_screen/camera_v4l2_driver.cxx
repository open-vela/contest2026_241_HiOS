/*
 * NuttX V4L2 camera driver for CameraScreen.
 *
 * Opens /dev/video0 (OV2710 RAW10 1920x1080), demosaics BGGR Bayer to
 * BGR888, center-crops to the preview size, rotates 180° (panel mount),
 * and flushes the canvas buffer before LVGL reads it.
 */

#include "camera_v4l2_driver.h"

#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"

#include <metalio/metalio.h>
#include <nuttx/cache.h>
#include <nuttx/video/video.h>

#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

#ifndef tskIDLE_PRIORITY
#define tskIDLE_PRIORITY 0
#endif

namespace {

constexpr const char *TAG = "CamV4L2";

constexpr const char *kVideoDev   = "/dev/video0";
constexpr int       kSrcW       = 1920;
constexpr int       kSrcH       = 1080;
constexpr int       kBufCount   = 3;
constexpr int       kPollMs     = 2000;

constexpr int kPowerSettleMs = 200;

inline void flush_cpu_writes(uint8_t *buf, size_t len)
{
    up_flush_dcache(reinterpret_cast<uintptr_t>(buf),
                    reinterpret_cast<uintptr_t>(buf + len));
}

inline void invalidate_cpu_reads(const uint8_t *buf, size_t len)
{
    up_invalidate_dcache(reinterpret_cast<uintptr_t>(buf),
                        reinterpret_cast<uintptr_t>(buf + len));
}

inline size_t raw10_frame_bytes(int width, int height)
{
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 10 / 8;
}

inline int raw10_row_stride(int width)
{
    return (width / 4) * 5;
}

inline uint16_t raw10_at(const uint8_t *raw, int width, int x, int y)
{
    const int groups     = width / 4;
    const int row_stride = groups * 5;
    const uint8_t *row   = raw + y * row_stride;
    const int gx         = x / 4;
    const int px         = x % 4;
    const uint8_t *g     = row + gx * 5;
    const uint8_t hi     = g[px];
    const uint8_t lo     = g[4];
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(hi) << 2) | ((lo >> (px * 2)) & 0x03));
}

inline uint16_t raw10_clamped(const uint8_t *raw, int width, int height,
                              int x, int y)
{
    if (x < 0) {
        x = 0;
    } else if (x >= width) {
        x = width - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= height) {
        y = height - 1;
    }
    return raw10_at(raw, width, x, y);
}

inline uint8_t to_u8(uint16_t v)
{
    return static_cast<uint8_t>(v >> 2);
}

void bggr_to_rgb(const uint8_t *raw, int width, int height, int x, int y,
                 uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
    const bool row_even = (y & 1) == 0;
    const bool col_even = (x & 1) == 0;

    if (row_even && col_even) {
        *out_b = to_u8(raw10_at(raw, width, x, y));
        *out_g = to_u8((raw10_clamped(raw, width, height, x - 1, y) +
                        raw10_clamped(raw, width, height, x + 1, y) +
                        raw10_clamped(raw, width, height, x, y - 1) +
                        raw10_clamped(raw, width, height, x, y + 1)) /
                       4);
        *out_r = to_u8((raw10_clamped(raw, width, height, x - 1, y - 1) +
                        raw10_clamped(raw, width, height, x + 1, y - 1) +
                        raw10_clamped(raw, width, height, x - 1, y + 1) +
                        raw10_clamped(raw, width, height, x + 1, y + 1)) /
                       4);
    } else if (row_even && !col_even) {
        *out_g = to_u8(raw10_at(raw, width, x, y));
        *out_b = to_u8((raw10_clamped(raw, width, height, x - 1, y) +
                        raw10_clamped(raw, width, height, x + 1, y)) /
                       2);
        *out_r = to_u8((raw10_clamped(raw, width, height, x, y - 1) +
                        raw10_clamped(raw, width, height, x, y + 1)) /
                       2);
    } else if (!row_even && col_even) {
        *out_g = to_u8(raw10_at(raw, width, x, y));
        *out_r = to_u8((raw10_clamped(raw, width, height, x - 1, y) +
                        raw10_clamped(raw, width, height, x + 1, y)) /
                       2);
        *out_b = to_u8((raw10_clamped(raw, width, height, x, y - 1) +
                        raw10_clamped(raw, width, height, x, y + 1)) /
                       2);
    } else {
        *out_r = to_u8(raw10_at(raw, width, x, y));
        *out_g = to_u8((raw10_clamped(raw, width, height, x - 1, y) +
                        raw10_clamped(raw, width, height, x + 1, y) +
                        raw10_clamped(raw, width, height, x, y - 1) +
                        raw10_clamped(raw, width, height, x, y + 1)) /
                       4);
        *out_b = to_u8((raw10_clamped(raw, width, height, x - 1, y - 1) +
                        raw10_clamped(raw, width, height, x + 1, y - 1) +
                        raw10_clamped(raw, width, height, x - 1, y + 1) +
                        raw10_clamped(raw, width, height, x + 1, y + 1)) /
                       4);
    }
}

void raw10_to_bgr888_rotated(const uint8_t *raw, int src_w, int src_h,
                             int crop_x, int crop_y, int dst_w, int dst_h,
                             uint8_t *dst)
{
    for (int oy = 0; oy < dst_h; ++oy) {
        for (int ox = 0; ox < dst_w; ++ox) {
            const int sx = crop_x + (dst_w - 1 - ox);
            const int sy = crop_y + (dst_h - 1 - oy);
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            bggr_to_rgb(raw, src_w, src_h, sx, sy, &r, &g, &b);
            uint8_t *px = dst + (static_cast<size_t>(oy) * dst_w + ox) * 3;
            px[0] = b;
            px[1] = g;
            px[2] = r;
        }
    }
}

struct CaptureBuf {
    void  *start  = nullptr;
    size_t length = 0;
};

class NuttxV4l2CameraDriver : public CameraScreen::CameraDriver
{
public:
    static NuttxV4l2CameraDriver &Instance()
    {
        static NuttxV4l2CameraDriver inst;
        return inst;
    }

    esp_err_t StartStreaming(uint8_t *bgr_buf, int w, int h,
                             CameraScreen::FrameReadyCallback on_frame_ready,
                             void *user_data) override
    {
        if (bgr_buf == nullptr || w <= 0 || h <= 0) {
            return ESP_ERR_INVALID_ARG;
        }
        StopStreaming();

        ctx_.dst_buf         = bgr_buf;
        ctx_.dst_w           = w;
        ctx_.dst_h           = h;
        ctx_.on_frame_ready  = on_frame_ready;
        ctx_.user_data       = user_data;
        ctx_.crop_x          = (kSrcW > w) ? (kSrcW - w) / 2 : 0;
        ctx_.crop_y          = (kSrcH > h) ? (kSrcH - h) / 2 : 0;
        ctx_.running         = true;
        ctx_.capture_count   = 0;

        BaseType_t ok = xTaskCreate(StreamTask, "cam_v4l2", 24 * 1024, this,
                                    tskIDLE_PRIORITY + 3, &ctx_.task);
        if (ok != pdPASS) {
            ctx_.running = false;
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    esp_err_t StopStreaming() override
    {
        if (!ctx_.running && ctx_.task == 0) {
            return ESP_OK;
        }
        ctx_.running = false;
        ctx_.task    = 0;
        return ESP_OK;
    }

    void SetFrameInterval(int interval) override
    {
        if (interval < 1) {
            interval = 1;
        }
        if (interval > 4) {
            interval = 4;
        }
        ctx_.interval = interval;
    }

private:
    struct Ctx {
        uint8_t *dst_buf = nullptr;
        int dst_w = 0;
        int dst_h = 0;
        int crop_x = 0;
        int crop_y = 0;
        CameraScreen::FrameReadyCallback on_frame_ready = nullptr;
        void *user_data = nullptr;
        volatile bool running = false;
        TaskHandle_t task = 0;
        int interval = 2;
        uint32_t capture_count = 0;
    } ctx_;

    NuttxV4l2CameraDriver() = default;

    static void StreamTask(void *arg)
    {
        static_cast<NuttxV4l2CameraDriver *>(arg)->StreamLoop();
        vTaskDelete(0);
    }

    void StreamLoop()
    {
        Ctx &c = ctx_;
        CaptureBuf bufs[kBufCount] = {};
        int fd = -1;
        bool streaming = false;

        if (metalio_camera_power(true) < 0) {
            ESP_LOGE(TAG, "camera power on failed");
            goto done;
        }
        vTaskDelayMs(kPowerSettleMs);

        fd = open(kVideoDev, O_RDWR);
        if (fd < 0) {
            ESP_LOGE(TAG, "open %s failed", kVideoDev);
            goto power_off;
        }

        {
            struct v4l2_format fmt = {};
            fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            fmt.fmt.pix.width       = kSrcW;
            fmt.fmt.pix.height      = kSrcH;
            fmt.fmt.pix.field       = V4L2_FIELD_ANY;
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10;
            if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
                ESP_LOGE(TAG, "VIDIOC_S_FMT failed");
                goto close_fd;
            }
        }

        {
            struct v4l2_requestbuffers req = {};
            req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_USERPTR;
            req.count  = kBufCount;
            req.mode   = V4L2_BUF_MODE_RING;
            if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
                ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
                goto close_fd;
            }
        }

        {
            const size_t frame_bytes = raw10_frame_bytes(kSrcW, kSrcH);
            for (int i = 0; i < kBufCount; ++i) {
                bufs[i].length = frame_bytes;
                bufs[i].start  = memalign(64, frame_bytes);
                if (bufs[i].start == nullptr) {
                    ESP_LOGE(TAG, "memalign frame buffer failed");
                    goto free_bufs;
                }
            }
        }

        for (int i = 0; i < kBufCount; ++i) {
            struct v4l2_buffer vb = {};
            vb.type      = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            vb.memory    = V4L2_MEMORY_USERPTR;
            vb.index     = i;
            vb.m.userptr = reinterpret_cast<uintptr_t>(bufs[i].start);
            vb.length    = bufs[i].length;
            if (ioctl(fd, VIDIOC_QBUF, &vb) < 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed", i);
                goto free_bufs;
            }
        }

        {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
                ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
                goto free_bufs;
            }
            streaming = true;
        }

        ESP_LOGI(TAG, "streaming %dx%d RAW10 -> %dx%d BGR888", kSrcW, kSrcH,
                 c.dst_w, c.dst_h);

        /* Allow AE / first frame to settle after stream-on. */
        vTaskDelayMs(150);

        {
            struct pollfd pfd = {};
            pfd.fd     = fd;
            pfd.events = POLLIN;
            const size_t dst_bytes =
                static_cast<size_t>(c.dst_w) * c.dst_h * 3;

            while (c.running) {
                pfd.revents = 0;
                const int pret = poll(&pfd, 1, kPollMs);
                if (pret <= 0) {
                    continue;
                }

                struct v4l2_buffer vb = {};
                vb.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                vb.memory = V4L2_MEMORY_USERPTR;
                if (ioctl(fd, VIDIOC_DQBUF, &vb) < 0) {
                    ESP_LOGW(TAG, "VIDIOC_DQBUF failed");
                    continue;
                }

                const uint32_t interval =
                    static_cast<uint32_t>(c.interval < 1 ? 1 : c.interval);
                const bool should_render =
                    ((c.capture_count++ % interval) == 0);

                if (should_render && vb.m.userptr != 0 && vb.length > 0) {
                    const uint8_t *src =
                        reinterpret_cast<const uint8_t *>(vb.m.userptr);
                    invalidate_cpu_reads(src, vb.length);
                    raw10_to_bgr888_rotated(src, kSrcW, kSrcH, c.crop_x,
                                            c.crop_y, c.dst_w, c.dst_h,
                                            c.dst_buf);
                    flush_cpu_writes(c.dst_buf, dst_bytes);
                    if (c.on_frame_ready != nullptr) {
                        c.on_frame_ready(c.user_data);
                    }
                }

                if (ioctl(fd, VIDIOC_QBUF, &vb) < 0) {
                    ESP_LOGW(TAG, "VIDIOC_QBUF failed");
                }
            }
        }

        if (streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd, VIDIOC_STREAMOFF, &type);
        }

    free_bufs:
        for (int i = 0; i < kBufCount; ++i) {
            if (bufs[i].start != nullptr) {
                free(bufs[i].start);
                bufs[i].start = nullptr;
            }
        }

    close_fd:
        if (fd >= 0) {
            close(fd);
        }

    power_off:
        metalio_camera_power(false);

    done:
        c.task = 0;
        ESP_LOGI(TAG, "stream task exit");
    }
};

struct DriverRegistrar {
    DriverRegistrar()
    {
        CameraScreen::SetCameraDriver(&NuttxV4l2CameraDriver::Instance());
    }
};

static DriverRegistrar s_driver_reg;

}  // namespace

CameraScreen::CameraDriver *GetNuttxV4l2CameraDriver()
{
    return &NuttxV4l2CameraDriver::Instance();
}
