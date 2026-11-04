// SPDX-License-Identifier: Apache-2.0
// Progressive JPEG fallback, isolated from camera and album decoders.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
// This NuttX target uses pthread TLS keys, not ELF __thread/tp storage.
// The private decoder is called only by the single browser-image worker.
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 8192
#include "../third_party/stb/stb_image.h"
unsigned char *hi_web_jpeg(const unsigned char *data,int size,unsigned *w,unsigned *h)
{
    int width,height,components;
    if(!stbi_info_from_memory(data,size,&width,&height,&components)||width<1||height<1||
       (size_t)width*height>1024*1024)return NULL;
    unsigned char *pixels=stbi_load_from_memory(data,size,&width,&height,&components,4);
    if(pixels){*w=width;*h=height;}return pixels;
}
