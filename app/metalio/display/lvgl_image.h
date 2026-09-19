/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL image wrapper — ported from MetalioClaw4 main/display/lvgl_display/lvgl_image.h.
 */

#pragma once

#include <lvgl.h>
#include <cstdint>
#include <cstddef>

/* Base class: wraps lv_img_dsc_t */
class LvglImage
{
public:
    virtual const lv_img_dsc_t *image_dsc() const = 0;
    virtual bool IsGif() const { return false; }
    virtual ~LvglImage() = default;
};

/* Raw image with pre-allocated data (caller owns the data) */
class LvglRawImage : public LvglImage
{
public:
    LvglRawImage(void *data, size_t size);
    virtual const lv_img_dsc_t *image_dsc() const override { return &image_dsc_; }
    virtual bool IsGif() const;

private:
    lv_img_dsc_t image_dsc_;
};

/* CBin image (stub: cbin_img_dsc not available in NuttX) */
class LvglCBinImage : public LvglImage
{
public:
    LvglCBinImage(void *data);
    virtual ~LvglCBinImage();
    virtual const lv_img_dsc_t *image_dsc() const override { return image_dsc_; }

private:
    lv_img_dsc_t *image_dsc_ = nullptr;
};

/* Source image: wraps an existing lv_img_dsc_t pointer */
class LvglSourceImage : public LvglImage
{
public:
    LvglSourceImage(const lv_img_dsc_t *image_dsc) : image_dsc_(image_dsc) {}
    virtual const lv_img_dsc_t *image_dsc() const override { return image_dsc_; }

private:
    const lv_img_dsc_t *image_dsc_;
};

/* Allocated image: owns and frees the data buffer */
class LvglAllocatedImage : public LvglImage
{
public:
    LvglAllocatedImage(void *data, size_t size);
    LvglAllocatedImage(void *data, size_t size, int width, int height,
                       int stride, int color_format);
    virtual ~LvglAllocatedImage();
    virtual const lv_img_dsc_t *image_dsc() const override { return &image_dsc_; }

private:
    lv_img_dsc_t image_dsc_;
};
