// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void hv_yuv420_rgb565(const uint8_t*y,const uint8_t*u,const uint8_t*v,unsigned stride,unsigned crop_x,unsigned crop_y,unsigned source_w,unsigned source_h,uint16_t*out,unsigned w,unsigned h);
#ifdef HI_VIDEO_TARGET_TEST
void hv_yuv420_rgb565_reference(const uint8_t*,const uint8_t*,const uint8_t*,unsigned,unsigned,unsigned,unsigned,unsigned,uint16_t*,unsigned,unsigned);
#endif
#ifdef __cplusplus
}
#endif
