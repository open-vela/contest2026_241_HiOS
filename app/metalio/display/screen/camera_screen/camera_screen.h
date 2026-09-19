/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * CameraScreen — ported from MetalioClaw4
 * main/display/screen/camera_screen/camera_screen.h.
 *
 * 720x720 fullscreen camera app. Screen layout (preserved verbatim):
 *   - Top 720x600: live preview / gallery grid / large photo viewer
 *   - Bottom 720x120: capture button + gallery tab button
 *   - Gallery mode: full-screen + 90px header (back + "相册" title)
 *
 * The original drives an OV2710 MIPI-CSI sensor through V4L2
 * (esp_video_init / esp_cam_sensor_xclk / VIDIOC_* ioctls), uses
 * IOExpander::CAM_PWDN for power control, esp_lv_adapter_lock for
 * cross-thread LVGL access, image_to_jpeg for JPEG encoding, and
 * heap_caps_aligned_alloc for PSRAM-backed canvas buffer.
 *
 * openvela has none of those (no V4L2 camera driver, no IOExpander,
 * no esp_lv_adapter, no image_to_jpeg, no heap_caps), so the camera
 * capture plane is stubbed behind a clean CameraDriver interface:
 *
 *   - StartStreaming(buf, w, h, on_frame_ready): begin filling `buf`
 *     with BGR888 frames; call `on_frame_ready` (on any thread) after
 *     each frame is written so the UI can invalidate the canvas.
 *   - StopStreaming(): stop the capture source.
 *
 * The default StubCameraDriver paints a slow-moving placeholder
 * gradient into the canvas via a low-priority FreeRTOS task, so the
 * preview visibly updates, the "拍照" button freezes a real frame,
 * and the gallery / viewer / delete flows are testable end-to-end.
 *
 * JPEG encoding is also stubbed (StubJpegEncoder returns false), which
 * matches the original's "编码失败，未保存" path. Plug in a real
 * JpegEncoder when an encoder is available — no UI changes needed.
 *
 * SD card / gallery code uses POSIX (opendir/readdir/unlink/fopen),
 * which NuttX provides natively. LVGL image loads use the "S:" POSIX
 * driver letter, wired up by the LVGL NuttX integration.
 */

#ifndef METALIO_CAMERA_SCREEN_H
#define METALIO_CAMERA_SCREEN_H

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"
#include "esp_err_shim.h"

#include <cstdint>
#include <cstddef>

class CameraScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "camera"; }

    // Legacy static entry point — kept for home_screen / metalio_main
    // callers that still use the original MetalioClaw4 signature.
    static lv_obj_t *CreateStatic();

    // Lifecycle hook: original toggled IOExpander::CAM_PWDN and started
    // / stopped the V4L2 capture worker. openvela forwards to the
    // configured CameraDriver (default stub) plus a log line for the
    // PWDN transition.
    static void LifecycleCallback(screen_lifecycle_event_t event);

    // -----------------------------------------------------------------
    // CameraDriver — pluggable camera capture interface.
    //
    // All methods may be invoked from the LVGL thread (LifecycleCallback
    // is called from the LVGL event handler). StartStreaming is
    // non-blocking: the driver is expected to spawn its own worker
    // task / timer / DMA pipeline and call `on_frame_ready` (which is
    // safe to call from any thread — internally it hops into LVGL via
    // lv_async_call / lv_obj_invalidate).
    // -----------------------------------------------------------------
    typedef void (*FrameReadyCallback)(void *user_data);

    class CameraDriver
    {
    public:
        virtual ~CameraDriver() = default;

        // Begin filling `bgr_buf` (B-G-R byte order, w*h*3 bytes) with
        // live frames. The driver MUST not write past w*h*3 bytes and
        // SHOULD call `on_frame_ready` after each completed frame so
        // the UI invalidates the canvas and repaints.
        virtual esp_err_t StartStreaming(uint8_t *bgr_buf, int w, int h,
                                         FrameReadyCallback on_frame_ready,
                                         void *user_data) = 0;
        virtual esp_err_t StopStreaming() = 0;

        // Optional quality hint (1 = high, 2 = medium, 4 = low). Default
        // implementation is a no-op; stub drivers can ignore it.
        virtual void SetFrameInterval(int /*interval*/) {}
    };

    // -----------------------------------------------------------------
    // JpegEncoder — pluggable BGR888 -> JPEG encoder.
    //
    // The original used `image_to_jpeg` from jpg/image_to_jpeg.h, which
    // wraps the ESP-IDF JPEG library. openvela has no such helper yet,
    // so the default StubJpegEncoder returns false (matching the
    // original's "编码失败，未保存" status path). Replace it with a
    // real encoder when one is available.
    // -----------------------------------------------------------------
    class JpegEncoder
    {
    public:
        virtual ~JpegEncoder() = default;

        // Encode `bgr` (w*h*3 bytes, B-G-R byte order) to JPEG at the
        // given quality (1-100). On success, returns true and sets
        // `*out` to a malloc'd buffer (caller frees with free()) and
        // `*out_len` to its size. On failure returns false and leaves
        // `*out` untouched.
        virtual bool Encode(const uint8_t *bgr, size_t bgr_size,
                            int w, int h, int quality,
                            uint8_t **out, size_t *out_len) = 0;
    };

    // Install a custom camera driver / JPEG encoder. Pass nullptr to
    // revert to the default stub. Must be called before Create() for
    // the change to take effect on the next screen instance.
    static void SetCameraDriver(CameraDriver *driver);
    static void SetJpegEncoder(JpegEncoder *encoder);

    // -----------------------------------------------------------------
    // PreviewBuffer — public surface kept for parity with the original
    // (the hardware test page reuses the camera pipeline on a custom
    // canvas). On openvela this just hands out the shared canvas
    // buffer; the caller is responsible for installing a CameraDriver
    // that will actually fill it.
    // -----------------------------------------------------------------
    struct PreviewBuffer {
        uint8_t *data;
        int      width;
        int      height;
    };

    static bool PreparePreviewBuffer(PreviewBuffer *out);
    static esp_err_t StartExternalPreview(lv_obj_t *canvas);
    static void StopExternalPreview();
};

#endif  // METALIO_CAMERA_SCREEN_H
