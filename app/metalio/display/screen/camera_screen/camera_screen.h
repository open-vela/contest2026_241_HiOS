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
 * Live preview comes from NuttxV4l2CameraDriver: /dev/video0, OV2710
 * RAW10 demosaiced to BGR888. CameraDriver is still pluggable:
 *
 *   - StartStreaming(buf, w, h, on_frame_ready): begin filling `buf`
 *     with BGR888 frames; call `on_frame_ready` (on any thread) after
 *     each frame is written so the UI can invalidate the canvas.
 *   - StopStreaming(): stop the capture source.
 *
 * If no driver has been installed, the screen binds the V4L2 driver
 * on first use. A failed stream leaves the canvas black and posts
 * "摄像头初始化失败".
 *
 * JPEG encoding falls back to StubJpegEncoder (returns false) when no
 * encoder is installed, matching the original's "编码失败，未保存"
 * path. camera_jpeg_encoder.cxx installs the stb encoder at startup.
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
#include <string>

class CameraScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "camera"; }

    // Legacy static entry point — kept for home_screen / metalio_main
    // callers that still use the original MetalioClaw4 signature.
    static lv_obj_t *CreateStatic();

    // Lifecycle hook: start / stop the configured CameraDriver
    // (default: OV2710 via /dev/video0).
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

        // Called only from the frame-ready callback, before the next frame.
        // Return true when the driver can safely write into this next buffer.
        virtual bool SetFrameBuffer(uint8_t * /*next*/) { return false; }


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
    // revert to the default (V4L2 camera, stub JPEG until the stb
    // encoder registers). Must be called before Create() for the
    // change to take effect on the next screen instance.
    static void SetCameraDriver(CameraDriver *driver);
    static void SetJpegEncoder(JpegEncoder *encoder);

    // Look-and-see shares the live camera preview and writes the analyzed
    // frame into the same SD-card root consumed by the camera gallery.
    static bool OpenLookAndSee(bool return_to_digital_people = false);
    static bool IsLookAndSeeMode();
    static unsigned VisionVisit();
    static void SetVisionService(const std::string &url,
                                 const std::string &token);
    static bool CaptureAndExplain(const std::string &question,
                                  std::string &answer,
                                  std::string &error);
    static bool CanAcceptPreviewFrame();
    static bool IsCapturing();
    // True only after the owning camera task has joined its capture driver.
    static bool PreviewStopped();

    // -----------------------------------------------------------------
    // PreviewBuffer — public surface kept for parity with the original
    // (the hardware test page reuses the camera pipeline on a custom
    // canvas). On openvela this just hands out the shared canvas
    // buffer. StartExternalPreview fills it from the active camera
    // driver (OV2710 /dev/video0 by default).
    // -----------------------------------------------------------------
    struct PreviewBuffer {
        uint8_t *data;
        int      width;
        int      height;
    };

    static bool PreparePreviewBuffer(PreviewBuffer *out);
    static esp_err_t StartExternalPreview(lv_obj_t *canvas);
    static void StopExternalPreview();
    typedef void (*PreviewObserver)(const uint8_t *bgr, int width, int height);
    static void SetPreviewObserver(PreviewObserver observer);
};

#endif  // METALIO_CAMERA_SCREEN_H
