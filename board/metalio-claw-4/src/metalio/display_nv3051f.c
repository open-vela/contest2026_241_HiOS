/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/metalio/display_nv3051f.c
 *
 * Metalio Claw4 720x720 MIPI-DSI panel bring-up (NV3051F).
 *
 * This drives the physical panel through NuttX's generic MIPI-DSI host API
 * (arch/risc-v/src/common/espressif/esp_mipi_dsi.c), ported from the
 * original CloudZao/MetalioClaw4 firmware:
 *   main/boards/metalio-claw-4/esp_lcd_nv3051f.c
 *   main/boards/metalio-claw-4/metalio-claw-4.cc
 *
 * Sequence:
 *   1. Power VDD_MIPI_DPHY (on-chip LDO channel 3 @ 2500 mV)
 *   2. Register the Espressif MIPI-DSI host (2 lanes @ 1000 Mbps)
 *   3. Hardware-reset the LCD (GPIO3)
 *   4. Configure DPI timing (720x720 RGB565, 36 MHz -> ~60 Hz)
 *   5. Send the NV3051F vendor DCS init table
 *   6. Bind a PSRAM framebuffer, exit sleep, start video, display on
 *   7. Register /dev/fb0 (LVGL renders into it, FBIO_UPDATE flushes cache)
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/video/fb.h>
#include <nuttx/video/mipi_dsi.h>
#include <nuttx/kmalloc.h>
#include <nuttx/arch.h>

#include <syslog.h>
#include <string.h>
#include <errno.h>

#include <arch/board/board.h>

#include "espressif/esp_mipi_dsi.h"
#include "espressif/esp_ldo.h"
#include "espressif/esp_gpio.h"

#ifdef CONFIG_ESPRESSIF_MIPI_DSI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define METALIO_FB_BPP      24
#define METALIO_FB_WIDTH    BOARD_DISPLAY_WIDTH
#define METALIO_FB_HEIGHT   BOARD_DISPLAY_HEIGHT
#define METALIO_FB_STRIDE   (METALIO_FB_WIDTH * (METALIO_FB_BPP / 8))
#define METALIO_FB_SIZE     (METALIO_FB_STRIDE * METALIO_FB_HEIGHT)

/* Triple buffering (num_fbs = 3, aligned with the MetalioClaw4 reference
 * firmware): LVGL renders directly into a back buffer and pans at the frame
 * boundary, so no off-screen memcpy is needed and the panel never shows a
 * buffer while the CPU is still writing it (no tearing, even with a
 * one-frame lookahead).
 */

#define METALIO_FB_COUNT    3
#define METALIO_FB_TOTAL    (METALIO_FB_SIZE * METALIO_FB_COUNT)

#define METALIO_LDO_MIPI_PHY_CHAN       3
#define METALIO_LDO_MIPI_PHY_VOLTAGE_MV 2500

#define METALIO_DSI_LANES          BOARD_LCD_MIPI_DSI_LANES
#define METALIO_DSI_LANE_BITRATE   1000

/* NV3051F vendor init table — every entry carries a single parameter byte.
 * Source: TRULY 3.95" (HE396-040T2BZZ) + NV3051F, GAMMA2.2 / 20250708. */

struct metalio_init_cmd_s
{
  uint8_t cmd;
  uint8_t data;
};

static const struct metalio_init_cmd_s g_nv3051f_init[] =
{
  /* Page 1 */
  {0xff, 0x30}, {0xff, 0x52}, {0xff, 0x01},
  {0xe3, 0x00},
  {0x20, 0x90}, {0x28, 0x5f}, {0x29, 0x02}, {0x2a, 0xcf},
  {0x30, 0x58}, {0x37, 0x9c}, {0x38, 0xa7}, {0x39, 0x43},
  {0x44, 0x00}, {0x49, 0x3c}, {0x59, 0xfe}, {0x5c, 0x00},
  {0x80, 0x20}, {0x91, 0x77}, {0x92, 0x77},
  {0xa0, 0x55}, {0xa1, 0x50}, {0xa3, 0x58}, {0xa4, 0x9c},
  {0xa7, 0x02}, {0xa8, 0x01}, {0xa9, 0x21}, {0xaa, 0xfc},
  {0xab, 0x28}, {0xac, 0x06}, {0xad, 0x06}, {0xae, 0x06},
  {0xaf, 0x03}, {0xb0, 0x08}, {0xb1, 0x26}, {0xb2, 0x28},
  {0xb3, 0x28}, {0xb4, 0x03}, {0xb5, 0x08}, {0xb6, 0x26},
  {0xb7, 0x08}, {0xb8, 0x26},
  {0xc0, 0x00}, {0xc1, 0x00}, {0xc2, 0x00}, {0xc3, 0x0f},

  /* Page 2 */
  {0xff, 0x30}, {0xff, 0x52}, {0xff, 0x02},
  {0xb5, 0x37}, {0xb1, 0x0b}, {0xb2, 0x0a}, {0xb3, 0x2f},
  {0xb4, 0x30}, {0xb0, 0x02}, {0xb6, 0x15}, {0xb7, 0x37},
  {0xb8, 0x0b}, {0xb9, 0x02}, {0xba, 0x0f}, {0xbb, 0x0f},
  {0xbc, 0x10}, {0xbd, 0x12}, {0xbe, 0x18}, {0xbf, 0x0f},
  {0xc0, 0x17}, {0xc1, 0x05}, {0xd5, 0x32}, {0xd1, 0x07},
  {0xd2, 0x06}, {0xd3, 0x2f}, {0xd4, 0x30}, {0xd0, 0x05},
  {0xd6, 0x13}, {0xd7, 0x37}, {0xd8, 0x0d}, {0xd9, 0x04},
  {0xda, 0x11}, {0xdb, 0x0f}, {0xdc, 0x10}, {0xdd, 0x12},
  {0xde, 0x1a}, {0xdf, 0x11}, {0xe0, 0x19}, {0xe1, 0x07},

  /* Page 3 */
  {0xff, 0x30}, {0xff, 0x52}, {0xff, 0x03},
  {0x08, 0x8a}, {0x09, 0x8b}, {0x0a, 0x88}, {0x0b, 0x89},
  {0x30, 0x00}, {0x31, 0x00}, {0x32, 0x00}, {0x33, 0x00},
  {0x34, 0xa1}, {0x35, 0x07}, {0x36, 0x60}, {0x37, 0x03},
  {0x40, 0x86}, {0x41, 0x87}, {0x42, 0x84}, {0x43, 0x85},
  {0x44, 0x22}, {0x45, 0xce}, {0x46, 0xcd}, {0x47, 0x22},
  {0x48, 0xd0}, {0x49, 0xcf},
  {0x50, 0x82}, {0x51, 0x83}, {0x52, 0x80}, {0x53, 0x81},
  {0x54, 0x22}, {0x55, 0xd2}, {0x56, 0xd1}, {0x57, 0x22},
  {0x58, 0xd4}, {0x59, 0xd3},
  {0x7e, 0x3c}, {0x7f, 0xc0},
  {0x80, 0x0c}, {0x81, 0x0d}, {0x82, 0x0f}, {0x83, 0x0f},
  {0x84, 0x0e}, {0x85, 0x06}, {0x86, 0x07}, {0x87, 0x04},
  {0x88, 0x05}, {0x89, 0x00}, {0x8a, 0x01}, {0x8b, 0x0f},
  {0x96, 0x0c}, {0x97, 0x0d}, {0x98, 0x0f}, {0x99, 0x0f},
  {0x9a, 0x0e}, {0x9b, 0x06}, {0x9c, 0x07}, {0x9d, 0x04},
  {0x9e, 0x05}, {0x9f, 0x00}, {0xa0, 0x01}, {0xa1, 0x0f},

  /* Page 0 */
  {0xff, 0x30}, {0xff, 0x52}, {0xff, 0x00},
  {0x36, 0x02},             /* MADCTL */
  {0x3a, 0x77},             /* COLMOD: RGB888 */
};

#define METALIO_INIT_COUNT (sizeof(g_nv3051f_init) / sizeof(g_nv3051f_init[0]))

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int metalio_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                FAR struct fb_videoinfo_s *vinfo);
static int metalio_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                                FAR struct fb_planeinfo_s *pinfo);
static int metalio_pandisplay(FAR struct fb_vtable_s *vtable,
                              FAR struct fb_planeinfo_s *pinfo);
static void metalio_frame_done(FAR void *arg);
#ifdef CONFIG_FB_UPDATE
static int metalio_updatearea(FAR struct fb_vtable_s *vtable,
                              FAR const struct fb_area_s *area);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct fb_vtable_s g_metalio_vtable =
{
  .getvideoinfo = metalio_getvideoinfo,
  .getplaneinfo = metalio_getplaneinfo,
  .pandisplay   = metalio_pandisplay,
#ifdef CONFIG_FB_UPDATE
  .updatearea   = metalio_updatearea,
#endif
};

static struct fb_videoinfo_s g_metalio_video =
{
  .fmt     = FB_FMT_RGB24,
  .xres    = METALIO_FB_WIDTH,
  .yres    = METALIO_FB_HEIGHT,
  .nplanes = 1,
};

static struct fb_planeinfo_s g_metalio_plane =
{
  .fbmem        = NULL,
  .fblen        = METALIO_FB_TOTAL,
  .stride       = METALIO_FB_STRIDE,
  .display      = 0,
  .bpp          = METALIO_FB_BPP,
  .xres_virtual = METALIO_FB_WIDTH,
  .yres_virtual = METALIO_FB_HEIGHT * METALIO_FB_COUNT,
  .xoffset      = 0,
  .yoffset      = 0,
};

static FAR uint8_t *g_metalio_fb;
static bool g_metalio_fb_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int metalio_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                FAR struct fb_videoinfo_s *vinfo)
{
  DEBUGASSERT(vtable != NULL && vtable == &g_metalio_vtable &&
              vinfo != NULL);
  memcpy(vinfo, &g_metalio_video, sizeof(*vinfo));
  return OK;
}

static int metalio_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                                FAR struct fb_planeinfo_s *pinfo)
{
  uint8_t display;

  DEBUGASSERT(vtable != NULL && vtable == &g_metalio_vtable &&
              pinfo != NULL);

  if (planeno != 0 || g_metalio_fb == NULL)
    {
      return -EINVAL;
    }

  display = pinfo->display;
  if (display >= METALIO_FB_COUNT)
    {
      return -EINVAL;
    }

  memcpy(pinfo, &g_metalio_plane, sizeof(*pinfo));

  /* Report each display buffer of the contiguous triple-buffer region.
   * display 0 exposes the full virtual buffer; display 1/2 expose the
   * second/third pages so LVGL can mmap them as its draw buffers.
   */

  pinfo->display = display;
  pinfo->fbmem   = g_metalio_fb + (size_t)display * METALIO_FB_SIZE;
  pinfo->fblen   = (display == 0) ? METALIO_FB_TOTAL : METALIO_FB_SIZE;
  pinfo->yoffset = (uint32_t)display * METALIO_FB_HEIGHT;

  return OK;
}

static int metalio_pandisplay(FAR struct fb_vtable_s *vtable,
                              FAR struct fb_planeinfo_s *pinfo)
{
  FAR void *target;

  DEBUGASSERT(vtable != NULL && vtable == &g_metalio_vtable &&
              pinfo != NULL);

  if (g_metalio_fb == NULL)
    {
      return -EAGAIN;
    }

  if (pinfo->yoffset >= (uint32_t)(METALIO_FB_HEIGHT * METALIO_FB_COUNT))
    {
      return -EINVAL;
    }

  target = g_metalio_fb + (size_t)pinfo->yoffset * METALIO_FB_STRIDE;
  return esp_mipi_dsi_switch_framebuffer(target);
}

static void metalio_frame_done(FAR void *arg)
{
  FAR struct fb_vtable_s *vtable = (FAR struct fb_vtable_s *)arg;

  fb_remove_paninfo(vtable, FB_NO_OVERLAY);
}

#ifdef CONFIG_FB_UPDATE
static int metalio_updatearea(FAR struct fb_vtable_s *vtable,
                              FAR const struct fb_area_s *area)
{
  size_t offset;
  size_t len;

  DEBUGASSERT(vtable != NULL && vtable == &g_metalio_vtable);

  if (g_metalio_fb == NULL)
    {
      return -EAGAIN;
    }

  if (area == NULL)
    {
      return esp_mipi_dsi_flush_framebuffer(g_metalio_fb, METALIO_FB_TOTAL);
    }

  if (area->y >= (METALIO_FB_HEIGHT * METALIO_FB_COUNT) ||
      area->x >= METALIO_FB_WIDTH)
    {
      return OK;
    }

  offset = (size_t)area->y * METALIO_FB_STRIDE +
           (size_t)area->x * (METALIO_FB_BPP / 8);
  len = (size_t)area->h * METALIO_FB_STRIDE;
  if (offset + len > METALIO_FB_TOTAL)
    {
      len = METALIO_FB_TOTAL - offset;
    }

  return esp_mipi_dsi_flush_framebuffer((FAR uint8_t *)g_metalio_fb + offset,
                                        len);
}
#endif /* CONFIG_FB_UPDATE */

static int metalio_send_init(FAR struct mipi_dsi_device *device)
{
  unsigned int i;

  for (i = 0; i < METALIO_INIT_COUNT; i++)
    {
      FAR const struct metalio_init_cmd_s *cmd = &g_nv3051f_init[i];
      ssize_t n = mipi_dsi_dcs_write(device, cmd->cmd, &cmd->data, 1);

      if (n < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: NV3051F DCS 0x%02x failed: %zd (%u/%u)\n",
                 cmd->cmd, n, i + 1, METALIO_INIT_COUNT);
          return (int)n;
        }
    }

  return OK;
}

static FAR struct mipi_dsi_device *metalio_nv3051f_initialize(
      FAR struct mipi_dsi_host *host)
{
  FAR struct mipi_dsi_device *device;
  int ret;

  if (host == NULL)
    {
      return NULL;
    }

  device = mipi_dsi_device_register(host, "nv3051f", 0);
  if (device == NULL)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_device_register failed\n");
      return NULL;
    }

  device->lanes = METALIO_DSI_LANES;
  device->format = MIPI_DSI_FMT_RGB888;
  device->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
                       MIPI_DSI_MODE_LPM;
  device->hs_rate = METALIO_DSI_LANE_BITRATE * 1000000UL;
  device->lp_rate = 0;

  ret = mipi_dsi_attach(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_attach failed: %d\n", ret);
      return NULL;
    }

  ret = metalio_send_init(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: NV3051F DCS init failed: %d\n", ret);
      return NULL;
    }

  return device;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int metalio_display_initialize(void)
{
  struct esp_mipi_dsi_bus_config_s bus_cfg;
  struct esp_mipi_dsi_dpi_config_s dpi;
  struct esp_ldo_config_t ldo;
  FAR struct mipi_dsi_host *host;
  FAR struct mipi_dsi_device *device;
  int ret;

  if (g_metalio_fb_ready)
    {
      return OK;
    }

  /* 1. Power VDD_MIPI_DPHY (LDO channel 3 @ 2500 mV). */

  ldo.chan_id = METALIO_LDO_MIPI_PHY_CHAN;
  ldo.voltage_mv = METALIO_LDO_MIPI_PHY_VOLTAGE_MV;
  ldo.handler = NULL;

  ret = esp_ldo_channel_acquire(&ldo);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: MIPI PHY LDO acquire failed: %d\n", ret);
      return ret;
    }

  /* 2. Register the MIPI-DSI host (2 lanes @ 1000 Mbps). */

  bus_cfg.num_data_lanes = METALIO_DSI_LANES;
  bus_cfg.lane_bit_rate_mbps = METALIO_DSI_LANE_BITRATE;

  ret = esp_mipi_dsi_initialize(&bus_cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: MIPI-DSI host init failed: %d\n", ret);
      return ret;
    }

  /* 3. Hardware-reset the LCD panel (GPIO3, active-low). */

  esp_configgpio(BOARD_LCD_RST_GPIO, OUTPUT);
  esp_gpiowrite(BOARD_LCD_RST_GPIO, 0);
  up_mdelay(20);
  esp_gpiowrite(BOARD_LCD_RST_GPIO, 1);
  up_mdelay(120);

  /* 4. Allocate the triple-buffered RGB888 framebuffer in PSRAM
   *    (DMA-capable).  Three contiguous frames; LVGL renders into a back
   *    buffer and pans to it at each frame boundary.
   */

  g_metalio_fb = kumm_memalign(64, METALIO_FB_TOTAL);
  if (g_metalio_fb == NULL)
    {
      syslog(LOG_ERR,
             "ERROR: FB alloc failed (%u bytes; enable PSRAM)\n",
             (unsigned int)METALIO_FB_TOTAL);
      ret = -ENOMEM;
      goto errout;
    }

  memset(g_metalio_fb, 0, METALIO_FB_TOTAL);
  g_metalio_plane.fbmem = g_metalio_fb;

  /* 5. Configure DPI timing (720x720 RGB888 @ 36 MHz -> ~60 Hz).
   *    Match NV3051F / MetalioClaw4 datasheet (esp_lcd_nv3051f.h).
   *    33 MHz was under-clocked and looked like panel undervoltage flicker
   *    on home/app paints; 48 MHz worsened swipe tear on this NuttX path. */

  memset(&dpi, 0, sizeof(dpi));
  dpi.h_res = METALIO_FB_WIDTH;
  dpi.v_res = METALIO_FB_HEIGHT;
  dpi.hsync_pulse_width = 2;
  dpi.hsync_back_porch = 44;
  dpi.hsync_front_porch = 46;
  dpi.vsync_pulse_width = 2;
  dpi.vsync_back_porch = 14;
  dpi.vsync_front_porch = 16;
  dpi.dpi_clock_freq_mhz = 36;
  dpi.virtual_channel = 0;
  dpi.format = MIPI_DSI_FMT_RGB888;

  ret = esp_mipi_dsi_configure_dpi(&dpi);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: configure_dpi failed: %d\n", ret);
      goto errout_fb;
    }

  /* 6. Initialize the NV3051F panel (vendor DCS sequence). */

  host = esp_mipi_dsi_host_get();
  device = metalio_nv3051f_initialize(host);
  if (device == NULL)
    {
      ret = -EIO;
      goto errout_fb;
    }

  /* 7. Bind the framebuffer to the DSI bridge (DW-GDMA). */

  ret = esp_mipi_dsi_bind_framebuffer(g_metalio_fb, METALIO_FB_SIZE,
                                      METALIO_FB_WIDTH, METALIO_FB_HEIGHT,
                                      METALIO_FB_BPP);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bind_framebuffer failed: %d\n", ret);
      goto errout_fb;
    }

  /* 8. Exit sleep, then start video streaming. */

  ret = mipi_dsi_dcs_exit_sleep_mode(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: exit_sleep_mode failed: %d\n", ret);
      goto errout_fb;
    }

  up_mdelay(120);

  ret = esp_mipi_dsi_video_start();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: video_start failed: %d\n", ret);
      goto errout_fb;
    }

  ret = mipi_dsi_dcs_set_display_on(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: set_display_on failed: %d\n", ret);
      goto errout_fb;
    }

  /* 9. Register /dev/fb0. */

  ret = fb_register_device(0, 0, &g_metalio_vtable);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: fb_register_device failed: %d\n", ret);
      goto errout_fb;
    }

  /* 10. Consume a queued FBIOPAN_DISPLAY request on every completed frame
   *     so the framebuffer POLLOUT flow-control keeps advancing. */

  esp_mipi_dsi_register_frame_done_cb(metalio_frame_done, &g_metalio_vtable);

  g_metalio_fb_ready = true;
  syslog(LOG_INFO,
         "/dev/fb0 ready %ux%u RGB888 triple-buffered (NV3051F MIPI-DSI)\n",
         METALIO_FB_WIDTH, METALIO_FB_HEIGHT);

  return OK;

errout_fb:
  kumm_free(g_metalio_fb);
  g_metalio_fb = NULL;
  g_metalio_plane.fbmem = NULL;

errout:
  return ret;
}

#else /* !CONFIG_ESPRESSIF_MIPI_DSI */

int metalio_display_initialize(void)
{
  syslog(LOG_ERR,
         "Metalio display requires CONFIG_ESPRESSIF_MIPI_DSI\n");
  return -ENOSYS;
}

#endif /* CONFIG_ESPRESSIF_MIPI_DSI */
