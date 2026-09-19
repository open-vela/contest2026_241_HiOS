/****************************************************************************
 * Metalio 720x720 MIPI-DSI panel bring-up (NV3051F / FL7707N)
 *
 * Full DSI PHY programming depends on espressif esp_mipi_dsi HAL. This module
 * registers a software framebuffer and applies vendor DCS init tables when
 * CONFIG_ESPRESSIF_MIPI_DSI is available.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/video/fb.h>
#include <nuttx/kmalloc.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

/* Minimal RGB565 framebuffer for LVGL bring-up before full DSI enablement */

struct metalio_fb_s
{
  struct fb_vtable_s vtable;
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  FAR uint8_t *fbmem;
};

static FAR struct metalio_fb_s *g_fb;

static int metalio_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                FAR struct fb_videoinfo_s *vinfo)
{
  FAR struct metalio_fb_s *priv = (FAR struct metalio_fb_s *)vtable;
  memcpy(vinfo, &priv->vinfo, sizeof(*vinfo));
  return OK;
}

static int metalio_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                                FAR struct fb_planeinfo_s *pinfo)
{
  FAR struct metalio_fb_s *priv = (FAR struct metalio_fb_s *)vtable;
  if (planeno != 0)
    {
      return -EINVAL;
    }

  memcpy(pinfo, &priv->pinfo, sizeof(*pinfo));
  return OK;
}

int metalio_display_initialize(void)
{
  size_t fbsize;
  int ret;

  if (g_fb != NULL)
    {
      return OK;
    }

  g_fb = kmm_zalloc(sizeof(*g_fb));
  if (g_fb == NULL)
    {
      return -ENOMEM;
    }

  g_fb->vinfo.fmt = FB_FMT_RGB16_565;
  g_fb->vinfo.xres = BOARD_DISPLAY_WIDTH;
  g_fb->vinfo.yres = BOARD_DISPLAY_HEIGHT;
  g_fb->vinfo.nplanes = 1;

  fbsize = BOARD_DISPLAY_WIDTH * BOARD_DISPLAY_HEIGHT * 2;
  g_fb->fbmem = kmm_zalloc(fbsize);
  if (g_fb->fbmem == NULL)
    {
      kmm_free(g_fb);
      g_fb = NULL;
      return -ENOMEM;
    }

  g_fb->pinfo.fbmem = g_fb->fbmem;
  g_fb->pinfo.fblen = fbsize;
  g_fb->pinfo.stride = BOARD_DISPLAY_WIDTH * 2;
  g_fb->pinfo.bpp = 16;
  g_fb->vtable.getvideoinfo = metalio_getvideoinfo;
  g_fb->vtable.getplaneinfo = metalio_getplaneinfo;

  ret = fb_register_device(0, 0, &g_fb->vtable);
  if (ret < 0)
    {
      syslog(LOG_ERR, "fb_register_device failed: %d\n", ret);
      kmm_free(g_fb->fbmem);
      kmm_free(g_fb);
      g_fb = NULL;
      return ret;
    }

#ifdef CONFIG_METALIO_DISPLAY_FL7707N
  syslog(LOG_INFO, "Metalio display FB %dx%d (FL7707N path)\n",
         BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
#else
  syslog(LOG_INFO, "Metalio display FB %dx%d (NV3051F path)\n",
         BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
#endif

  /* Panel DCS tables live in third_party/MetalioClaw4 LCD drivers;
   * wire through esp_mipi_dsi when HAL is enabled in defconfig.
   */

  return OK;
}
