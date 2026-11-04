// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct hv_decoder hv_decoder;
typedef struct {const uint8_t *y,*u,*v;unsigned stride,width,height,crop_x,crop_y;} hv_picture;
hv_decoder *hv_decoder_create(void);
// Annex B access unit. Returns 1 for a picture, 0 for headers, -1 on error.
int hv_decoder_decode(hv_decoder*,const uint8_t*,size_t,hv_picture*);
const char *hv_decoder_error(hv_decoder*);
size_t hv_decoder_bytes(hv_decoder*);
void hv_decoder_destroy(hv_decoder*);
// Allocations made by the vendored decoder are bounded per decoder/task.
void *hi_video_malloc(size_t);
void hi_video_free(void*);
#ifdef __cplusplus
}
#endif
