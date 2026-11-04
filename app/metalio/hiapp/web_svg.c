// SPDX-License-Identifier: Apache-2.0
// NanoSVG remains private to browser images; no global LVGL renderer changes.
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "../third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvgrast.h"
#include <stdint.h>
int hi_web_svg(char *source,uint8_t **pixels,unsigned *width,unsigned *height)
{
    NSVGimage *svg=nsvgParse(source,"px",96.0f);
    if(!svg)return 0;
    float w=svg->width,h=svg->height;
    if(!isfinite(w)||!isfinite(h)||w<1||h<1||w>16384||h>16384){nsvgDelete(svg);return 0;}
    float scale=fminf(1.0f,fminf(636.0f/w,480.0f/h));
    unsigned ow=(unsigned)ceilf(w*scale),oh=(unsigned)ceilf(h*scale);
    uint8_t *p=(uint8_t*)calloc((size_t)ow*oh,4);
    NSVGrasterizer *r=p?nsvgCreateRasterizer():NULL;
    if(!r){free(p);nsvgDelete(svg);return 0;}
    nsvgRasterize(r,svg,0,0,scale,p,ow,oh,ow*4);
    nsvgDeleteRasterizer(r);nsvgDelete(svg);
    *pixels=p;*width=ow;*height=oh;return 1;
}
