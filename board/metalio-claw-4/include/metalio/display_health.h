/* SPDX-License-Identifier: Apache-2.0 */
#ifndef METALIO_DISPLAY_HEALTH_H
#define METALIO_DISPLAY_HEALTH_H
#include <nuttx/video/fb.h>
#include <stdint.h>
#define METALIO_FBIO_HEALTH  _FBIOC(0x80)
#define METALIO_FBIO_RECOVER _FBIOC(0x81)
#ifdef HI_STABILITY_PROBE
#define METALIO_FBIO_STALL _FBIOC(0x82)
#endif
struct metalio_display_health_s
{
  uint32_t frames;
  uint32_t underruns;
  uint32_t recoveries;
};
#endif
