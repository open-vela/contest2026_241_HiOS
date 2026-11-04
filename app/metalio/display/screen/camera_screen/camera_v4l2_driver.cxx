#include "hi_task_cpp.h"
/*
 * NuttX V4L2 camera driver for CameraScreen.
 *
 * Opens /dev/video0 (OV2710 RAW10 1920x1080), demosaics BGGR Bayer to
 * BGR888, center-crops to the preview size, rotates 180掳 (panel mount),
 * and flushes the canvas buffer before LVGL reads it.
 */

#include "camera_v4l2_driver.h"

#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "esp_heap_caps.h"
#include "freertos_shim.h"

#include <nuttx/video/video.h>

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <malloc.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <atomic>
#include <algorithm>
#include <pthread.h>
#include <sched.h>
#include <cstring>
#include <metalio/metalio.h>

#ifndef tskIDLE_PRIORITY
#define tskIDLE_PRIORITY 0
#endif

extern "C" void esp_mipi_csi_dump_status(void);
extern "C" void esp_mipi_dsi_dma_kick(void);

#ifdef HI_APP_SMP_PROBE
static bool g_camera_probe_baseline=false;
static std::atomic<unsigned>g_camera_probe_count{0};
static std::atomic<unsigned>g_camera_probe_cpus{0};
#endif
namespace {

constexpr const char *TAG = "CamV4L2";

constexpr const char *kVideoDev   = "/dev/video0";
constexpr int       kSrcW       = 1920;
constexpr int       kSrcH       = 1080;
/* Three buffers: the finished frame stays put while DMA fills the other.
 * A single ring buffer is handed straight back to DMA inside the
 * frame-done callback, which overwrites it and stalls the shared
 * display channel. */
constexpr int       kBufCount   = 3;
constexpr int       kStartWaitMs = 8000;


extern "C" int esp_cache_msync(void *address, size_t size, int flags);
constexpr int kCacheSyncDmaToCpu = 1 << 3;

inline void invalidate_cpu_reads(const uint8_t *buf, size_t len)
{
    while (len) {
        const size_t chunk = len > 4096 ? 4096 : len;
        (void)esp_cache_msync(const_cast<uint8_t *>(buf), chunk,
                              kCacheSyncDmaToCpu);
        buf += chunk;
        len -= chunk;
    }
}

inline size_t raw10_frame_bytes(int width, int height)
{
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 10 / 8;
}

inline int raw10_row_stride(int width)
{
    return (width / 4) * 5;
}

inline uint8_t to_u8(uint16_t v)
{
    return static_cast<uint8_t>(v >> 2);
}

// RAW Bayer values are linear; sending them directly to an sRGB panel/JPEG
// made midtones visibly too dark. One fixed monotonic LUT restores midtones
// identically for camera and Hi Vision, without lengthening sensor exposure,
// changing geometry, or adding another frame copy. Black/white stay 0/255.
static constexpr uint8_t kPreviewTone[256]={0,8,12,16,19,22,24,27,29,32,34,36,38,40,42,43,45,47,49,50,52,54,55,57,58,60,61,63,64,66,67,68,70,71,72,74,75,76,78,79,80,81,83,84,85,86,87,89,90,91,92,93,94,96,97,98,99,100,101,102,103,104,105,106,107,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,125,126,127,128,129,130,131,132,133,134,135,136,137,138,138,139,140,141,142,143,144,145,146,146,147,148,149,150,151,152,152,153,154,155,156,157,158,158,159,160,161,162,162,163,164,165,166,167,167,168,169,170,171,171,172,173,174,175,175,176,177,178,178,179,180,181,181,182,183,184,185,185,186,187,188,188,189,190,191,191,192,193,194,194,195,196,196,197,198,199,199,200,201,202,202,203,204,204,205,206,207,207,208,209,209,210,211,211,212,213,214,214,215,216,216,217,218,218,219,220,220,221,222,222,223,224,225,225,226,227,227,228,229,229,230,231,231,232,233,233,234,235,235,236,236,237,238,238,239,240,240,241,242,242,243,244,244,245,246,246,247,247,248,249,249,250,251,251,252,252,253,254,254,255};

/* BGGR 2x2 block -> one RGB pixel. Fast enough for 720x600 preview on P4. */

// Keep full RAW10 geometry and identical pixels. Optimize this hot scalar
// loop for throughput rather than the firmware-wide code-size setting.
#if defined(__GNUC__)
__attribute__((optimize("O3")))
#endif
void raw10_to_bgr888_rotated(const uint8_t *raw, int src_w, int src_h,
                             int crop_x, int crop_y, int dst_w, int dst_h,
                             uint8_t *dst, const int *x_offsets, const int *y_offsets)
{
    if (dst_w <= 0 || dst_w > 1920 || dst_h <= 0) return;
    hi_compute_scope compute_scope{};
#ifdef HI_APP_SMP_PROBE
    if(!g_camera_probe_baseline)
#endif
    hi_task_compute_begin(&compute_scope,0);
    struct Restore{hi_compute_scope *s;~Restore(){hi_task_compute_end(s);}} restore{&compute_scope};
#ifdef HI_APP_SMP_PROBE
    if(!g_camera_probe_baseline){g_camera_probe_count.fetch_add(1);g_camera_probe_cpus.fetch_or(1u<<hi_task_current_cpu());}
#endif
    const int stride = raw10_row_stride(src_w);
    const size_t output_stride = static_cast<size_t>(dst_w) * 3;
    int previous_sy = -1;
    for (int oy = 0; oy < dst_h; ++oy) {
#ifdef HI_APP_SMP_PROBE
        if(!g_camera_probe_baseline&&(oy&63)==0)g_camera_probe_cpus.fetch_or(1u<<hi_task_current_cpu());
#endif
        const int sy = y_offsets[oy];
        const uint8_t *row0 = raw + sy * stride;
        const uint8_t *row1 = row0 + stride;
        uint8_t *out = dst + static_cast<size_t>(oy) * output_stride;
        if (sy == previous_sy) {
            // The output can be taller than the Bayer cell grid. Reuse the
            // identical completed row instead of demosaicing it twice.
            std::memcpy(out, out - output_stride, output_stride);
            continue;
        }
        previous_sy = sy;
        // Only these two cropped Bayer rows are read. Invalidate their
        // aligned span immediately before conversion, rather than evicting
        // all 2.6 MB (including unused side margins) before the first pixel.
        // Keep the existing <=4 KiB cache-sync slices to protect I2S/DSI.
        const uintptr_t begin = reinterpret_cast<uintptr_t>(row0 + x_offsets[dst_w - 1]) & ~uintptr_t(63);
        const uintptr_t end = (reinterpret_cast<uintptr_t>(row1 + x_offsets[0] + 2) + 63) & ~uintptr_t(63);
        invalidate_cpu_reads(reinterpret_cast<const uint8_t *>(begin), end - begin);
        for (int ox = 0; ox < dst_w; ++ox) {
            const int offset = x_offsets[ox];
            // RAW10 stores each pixel's upper eight bits directly. The
            // discarded two bits do not affect an 8-bit BGR output.
            if (ox && offset==x_offsets[ox-1]) {
                out[0]=out[-3];out[1]=out[-2];out[2]=out[-1];out+=3;continue;
            }
            out[0] = kPreviewTone[row0[offset]];
            out[1] = kPreviewTone[(static_cast<unsigned>(row0[offset + 1]) + row1[offset]) / 2];
            out[2] = kPreviewTone[row1[offset + 1]];
            out += 3;
        }
    }
}

struct CaptureBuf {
    void  *start  = nullptr;
    size_t length = 0;
};

// Never interpret a short/error/unowned DMA result as a full RAW image.
// Keeping the last valid displayed frame is safer than exposing invalid pixels.
bool complete_raw_frame(const v4l2_buffer &vb, const CaptureBuf *bufs) {
    const size_t bytes=raw10_frame_bytes(kSrcW,kSrcH);
    return !(vb.flags & V4L2_BUF_FLAG_ERROR) && vb.index<kBufCount &&
           bufs[vb.index].start &&
           vb.m.userptr==reinterpret_cast<uintptr_t>(bufs[vb.index].start) &&
           vb.length==bufs[vb.index].length && vb.bytesused==bytes &&
           vb.length>=bytes;
}

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
        if (bgr_buf == nullptr || w <= 0 || w > kSrcW || h <= 0 || h > kSrcH) {
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
        ctx_.started         = false;
        ctx_.start_ok        = false;
        ctx_.capture_count   = 0;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 24 * 1024);
        int ret = hi_task_create_legacy(&thread_, &attr, StreamTask, this);
        pthread_attr_destroy(&attr);
        if (ret != 0) {
            ctx_.running = false;
            return ESP_FAIL;
        }

        joinable_ = true;
        int waited = 0;
        while (!ctx_.started && ctx_.running && waited < kStartWaitMs) {
            vTaskDelayMs(5);
            waited += 5;
        }
        if (!ctx_.start_ok) {
            ESP_LOGE(TAG, "STREAMON did not start");
            StopStreaming();
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    esp_err_t StopStreaming() override
    {
        ctx_.running = false;
        if (joinable_) {
            // Only the background camera owner calls this. Joining ensures
            // no old worker can touch the next session's buffers or context.
            pthread_join(thread_, nullptr);
            joinable_ = false;
        }
        return ESP_OK;
    }

    bool SetFrameBuffer(uint8_t *next) override
    {
        if (!next) return false;
        // Runs on StreamTask inside its callback, never concurrently with
        // conversion. The following frame will use this buffer.
        ctx_.dst_buf = next;
        return true;
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
        std::atomic<bool> running{false};
        std::atomic<bool> started{false};
        std::atomic<bool> start_ok{false};
        std::atomic<int> interval{1};
        uint32_t capture_count = 0;
    } ctx_;

    pthread_t thread_{};
    bool joinable_ = false;
    NuttxV4l2CameraDriver() = default;

    static void *StreamTask(void *arg)
    {
        // Only startup overlaps screen construction. Settle waits yield, and
        // conversion returns to its original priority before publishing frames.
        sched_param priority{};priority.sched_priority=CONFIG_METALIO_APP_PRIORITY+1;
        (void)pthread_setschedparam(pthread_self(),SCHED_RR,&priority);
        static_cast<NuttxV4l2CameraDriver *>(arg)->StreamLoop();
        return nullptr;
    }

    CaptureBuf capture_bufs_[kBufCount] = {}; // owned by this singleton; reused only after join

    void StreamLoop()
    {
        Ctx &c = ctx_;
        CaptureBuf *bufs = capture_bufs_;
        // Fixed geometry is mapped once per stream. All pixel positions,
        // Bayer averaging, rotation and output dimensions stay identical.
        int x_offsets[kSrcW],y_offsets[kSrcH];
        int crop_w=kSrcW,crop_h=kSrcW*c.dst_h/c.dst_w;
        if(crop_h>kSrcH){crop_h=kSrcH;crop_w=kSrcH*c.dst_w/c.dst_h;}
        const int crop_x=((kSrcW-crop_w)/2)&~1,crop_y=((kSrcH-crop_h)/2)&~1;
        for(int x=0;x<c.dst_w;++x){int sx=(crop_x+(c.dst_w-1-x)*crop_w/c.dst_w)&~1;
            if(sx>kSrcW-2)sx=kSrcW-2;x_offsets[x]=(sx/4)*5+sx%4;}
        for(int y=0;y<c.dst_h;++y){int sy=(crop_y+(c.dst_h-1-y)*crop_h/c.dst_h)&~1;
            y_offsets[y]=sy>kSrcH-2?kSrcH-2:sy;}

        int fd = -1;
        bool streaming = false;

        fd = open(kVideoDev, O_RDWR | O_NONBLOCK);
        if (fd < 0 && (errno == ENOENT || errno == ENODEV)) {
            const int recovered = metalio_camera_ensure_ready();
            if (recovered == 0) fd = open(kVideoDev, O_RDWR | O_NONBLOCK);
            else ESP_LOGW(TAG, "camera registration retry failed: %d", recovered);
        }
        if (fd < 0) {
            ESP_LOGE(TAG, "open %s failed errno=%d", kVideoDev, errno);
            goto done;
        }

        {
            struct v4l2_format fmt = {};
            fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            fmt.fmt.pix.width       = kSrcW;
            fmt.fmt.pix.height      = kSrcH;
            fmt.fmt.pix.field       = V4L2_FIELD_ANY;
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10;
            if (ioctl(fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0) {
                ESP_LOGE(TAG, "VIDIOC_S_FMT failed errno=%d", errno);
                goto close_fd;
            }
            if (fmt.fmt.pix.width != kSrcW || fmt.fmt.pix.height != kSrcH ||
                fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_SBGGR10) {
                ESP_LOGE(TAG, "OV2710 mode mismatch: %lux%lu format=%08lx",
                         static_cast<unsigned long>(fmt.fmt.pix.width),
                         static_cast<unsigned long>(fmt.fmt.pix.height),
                         static_cast<unsigned long>(fmt.fmt.pix.pixelformat));
                goto close_fd;
            }
        }

        {
            struct v4l2_requestbuffers req = {};
            req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_USERPTR;
            req.count  = kBufCount;
            req.mode   = V4L2_BUF_MODE_RING;
            if (ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req) < 0) {
                ESP_LOGE(TAG, "VIDIOC_REQBUFS failed errno=%d", errno);
                goto close_fd;
            }
        }

        {
            const size_t frame_bytes = raw10_frame_bytes(kSrcW, kSrcH);
            for (int i = 0; i < kBufCount; ++i) {
                bufs[i].length = frame_bytes;
                if (bufs[i].start == nullptr) {
                    bufs[i].start = heap_caps_aligned_alloc(
                        64, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
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
            if (ioctl(fd, VIDIOC_QBUF, (unsigned long)&vb) < 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed errno=%d", i, errno);
                goto free_bufs;
            }
        }

        {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(fd, VIDIOC_STREAMON, (unsigned long)&type) < 0) {
                ESP_LOGE(TAG, "VIDIOC_STREAMON failed errno=%d", errno);
                goto free_bufs;
            }
            streaming = true;
        }

        /* Sensor STREAMON already waits for its hardware settle/readback.
         * Resume display DMA now; a second unconditional 30 ms delay here
         * only postpones dequeuing the first completed sensor frame. */
        esp_mipi_dsi_dma_kick();

        // Conversion must progress while LVGL redraws the 720x600 canvas.
        // Keep it above UI but below audio and communication workers; each
        // empty DQBUF yields, so it cannot spin and starve the display.
        {sched_param normal{};normal.sched_priority=CONFIG_METALIO_APP_PRIORITY+2;
        (void)pthread_setschedparam(pthread_self(),SCHED_RR,&normal);}
        c.start_ok = true;
        c.started  = true;
        ESP_LOGI(TAG, "streaming %dx%d RAW10 -> %dx%d BGR888", kSrcW, kSrcH,
                 c.dst_w, c.dst_h);

        {
            int idle_ms = 0;
            unsigned cold_dark_skips=0, extreme_skips=0;
            bool published=false, published_white=false, white_pending=false;
            TickType_t white_since=0;
            int previous_mean=-1;

            /* DQBUF directly. This driver's poll() does not wake when a
             * frame is already queued, so waiting on poll left the preview
             * black with STREAMON already reported successful. */
            while (c.running) {
                struct v4l2_buffer vb = {};
                vb.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                vb.memory = V4L2_MEMORY_USERPTR;
                if (ioctl(fd, VIDIOC_DQBUF, (unsigned long)&vb) < 0) {
                    if (errno != EAGAIN) {
                        ESP_LOGW(TAG, "VIDIOC_DQBUF failed errno=%d", errno);
                    }
                    const int wait_ms = c.capture_count ? 5 : 2;
                    idle_ms += wait_ms;
                    if (idle_ms == 3000) {
                        ESP_LOGE(TAG, "no frame for 3s");
                        write(1, "CAM_NOFRAME\n", 12);
                        esp_mipi_csi_dump_status();
                    }
                    // This V4L2 lower-half has no usable POLLIN notification.
                    // A 20 ms sleep added 0-20 ms of jitter to each 40 ms
                    // sensor frame, making an otherwise live preview jerky.
                    vTaskDelayMs(wait_ms);
                    continue;
                }

                idle_ms = 0;
                // Preview needs the newest completed frame. If conversion
                // ran past a sensor period, return older queued frames first
                // instead of converting the whole backlog with visible lag.
                for(int stale=0;stale<kBufCount-1 && c.running;++stale) {
                    struct v4l2_buffer latest={};
                    latest.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    latest.memory=V4L2_MEMORY_USERPTR;
                    if(ioctl(fd,VIDIOC_DQBUF,(unsigned long)&latest)<0) break;
                    // A newer corrupt/incomplete frame must not discard a
                    // good frame already owned by this conversion pass.
                    if(!complete_raw_frame(latest,bufs)) {
                        latest.flags=0;
                        if(ioctl(fd,VIDIOC_QBUF,(unsigned long)&latest)<0)
                            ESP_LOGW(TAG,"return invalid frame failed errno=%d",errno);
                        continue;
                    }
                    vb.flags=0;
                    if(ioctl(fd,VIDIOC_QBUF,(unsigned long)&vb)<0) {
                        latest.flags=0;
                        (void)ioctl(fd,VIDIOC_QBUF,(unsigned long)&latest);
                        break;
                    }
                    vb=latest;
                }
                const uint32_t interval =
                    static_cast<uint32_t>(c.interval.load());
                const bool should_render =
                    ((c.capture_count++ % interval) == 0) &&
                    CameraScreen::CanAcceptPreviewFrame();

                if (should_render && complete_raw_frame(vb,bufs)) {
                    const uint8_t *src =
                        reinterpret_cast<const uint8_t *>(vb.m.userptr);
                    if (c.capture_count == 1) {
                        printf("CAM_RAW bytes=%lu", static_cast<unsigned long>(vb.bytesused));
                        for (int i = 0; i < 32; ++i) printf(" %02x", src[i]);
                        printf("\n");
                    }
                    raw10_to_bgr888_rotated(src, kSrcW, kSrcH, c.crop_x,
                                            c.crop_y, c.dst_w, c.dst_h,
                                            c.dst_buf,x_offsets,y_offsets);
                    // Sample the already converted CPU-owned frame. Do not
                    // alter image data, exposure, resolution or JPEG quality.
                    unsigned sum=0, extremes=0, count=0;
                    const size_t bytes=static_cast<size_t>(c.dst_w)*c.dst_h*3;
                    for (size_t i=0;i+2<bytes;i+=3*997) {
                        int y=(c.dst_buf[i]+c.dst_buf[i+1]+c.dst_buf[i+2])/3;
                        sum+=y;extremes+=(y>=254 || y<=1);++count;
                    }
                    unsigned whites=0, white_samples=0;
                    // Sample all parts of the frame, independently of row
                    // stride. Allow a few dark/noisy pixels in an otherwise
                    // clipped-white frame; requiring every sample to be 254
                    // missed the exposure flashes produced by rapid motion.
                    for (int gy=0;gy<16;++gy) for (int gx=0;gx<24;++gx) {
                        const int py=(gy*2+1)*c.dst_h/32;
                        const int px=(gx*2+1)*c.dst_w/48;
                        const uint8_t *pixel=c.dst_buf+
                            (static_cast<size_t>(py)*c.dst_w+px)*3;
                        ++white_samples;
                        whites+=(pixel[0]>=246 && pixel[1]>=246 && pixel[2]>=246);
                    }
                    const int mean=count?sum/count:0;
                    // At most two sensor periods for initial AE. A truly dark
                    // scene still becomes visible; there is no indefinite gate.
                    bool hold=!published && mean<8 && cold_dark_skips++<2;
                    if (published && count && extremes==count && previous_mean>=0 &&
                        std::abs(mean-previous_mean)>100 && extreme_skips++<2) hold=true;
                    const bool clipped_white=mean>=246 && whites*100>=white_samples*97;
                    // Returning from Gallery reopens the sensor and resets
                    // `published`. Its first clipped AE frame needs the same
                    // protection as a white flash during an existing stream.
                    const bool abrupt_white=(!published || !published_white) && clipped_white;
                    if (abrupt_white) {
                        const TickType_t now=xTaskGetTickCount();
                        if (!white_pending) {white_pending=true;white_since=now;}
                        // Keep the last complete frame while AE converges,
                        // but continue dequeuing/requeuing RAW buffers. Use
                        // elapsed time, not a two-frame allowance that leaked
                        // the third white frame. A genuinely sustained bright
                        // scene is admitted after 500 ms, so this cannot lock
                        // the preview on an old scene indefinitely.
                        if (static_cast<TickType_t>(now-white_since)<500) hold=true;
                    } else white_pending=false;
                    if (!hold) {
                        extreme_skips=0;white_pending=false;previous_mean=mean;published=true;
                        published_white=clipped_white;
                        if (c.on_frame_ready) c.on_frame_ready(c.user_data);
                    }
                }

                vb.flags = 0;
                if (ioctl(fd, VIDIOC_QBUF, (unsigned long)&vb) < 0) {
                    ESP_LOGW(TAG, "VIDIOC_QBUF failed errno=%d", errno);
                }
                // If conversion takes longer than a sensor period, DQBUF
                // can stay permanently ready. A real sleep (not sched_yield)
                // gives the lower-priority UI a bounded opportunity to draw
                // the latest frame instead of starving behind this producer.
                vTaskDelayMs(1);
            }
        }

        if (streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&type);
        }

    free_bufs:
        // Close the V4L2 session before reusing queued USERPTR memory,
        // including the partially queued/failed STREAMON paths.
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        // Retain the three bounded RAW buffers for the next camera session.
        // Repeated multi-megabyte allocation/free interleaved with LVGL screen
        // creation fragments the heap and eventually makes reopen fail.

    close_fd:
        if (fd >= 0) {
            close(fd);
        }

    done:
        c.started  = true;
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

#include "hi_camera_probe.inc"
