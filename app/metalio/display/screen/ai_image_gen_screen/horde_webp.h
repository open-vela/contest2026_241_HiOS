// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include "../../../third_party/libwebp/src/webp/decode.h"

namespace HiHorde {
// Caller owns malloc() output. Decode straight into bounded BGRA pixels for
// LVGL ARGB8888 on this little-endian device; no full-resolution RGBA copy.
inline uint8_t* DecodeWebP(const uint8_t* data,size_t size,int& width,int& height) {
    width=height=0;
    WebPDecoderConfig config;
    if(!WebPInitDecoderConfig(&config))return nullptr;
    if(WebPGetFeatures(data,size,&config.input)!=VP8_STATUS_OK ||
       config.input.has_animation || config.input.width<1 || config.input.height<1 ||
       config.input.width>512 || config.input.height>512)return nullptr;
    int w=config.input.width,h=config.input.height;
    if(w>450 || h>450) {
        if(w>=h){h=h*450/w;w=450;}else{w=w*450/h;h=450;}
        if(w<1)w=1;if(h<1)h=1;
        config.options.use_scaling=1;
        config.options.scaled_width=w;config.options.scaled_height=h;
    }
    config.options.use_threads=0;
    config.options.use_cropping=0;
    const size_t bytes=size_t(w)*h*4;
    uint8_t* pixels=static_cast<uint8_t*>(malloc(bytes));
    if(!pixels)return nullptr;
    config.output.colorspace=MODE_BGRA;
    config.output.is_external_memory=1;
    config.output.u.RGBA.rgba=pixels;
    config.output.u.RGBA.stride=w*4;
    config.output.u.RGBA.size=bytes;
    VP8StatusCode result=WebPDecode(data,size,&config);
    WebPFreeDecBuffer(&config.output);
    if(result!=VP8_STATUS_OK){free(pixels);return nullptr;}
    width=w;height=h;return pixels;
}
}
