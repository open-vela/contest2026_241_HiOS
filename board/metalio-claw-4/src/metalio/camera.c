/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/metalio/camera.c
 *
 * OV2710 MIPI-CSI image sensor — SCCB control link + XCLK + V4L2 capture.
 *
 * This registers a NuttX V4L2 capture device at /dev/video0 backed by:
 *   - an imgsensor lower-half (OV2710 SCCB control: power, XCLK, init
 *     register list, stream on/off), and
 *   - an imgdata lower-half (ESP32-P4 MIPI-CSI bridge -> DW-GDMA ch2 ->
 *     memory, feeding the generic V4L2 capture upper-half via
 *     complete_capture()).
 *
 * SCCB register access is 16-bit address + 8-bit data (reg_list_a16_d8),
 * matching the reference firmware's OV2710 usage.  The sensor runs in
 * MIPI_1lane_24Minput_RAW10_1920x1080_25fps: 1 data lane, 800 Mbps/lane,
 * 24 MHz XCLK, RAW10 Bayer BGGR output.
 *
 * XCLK is derived from the ESP clock router by sourcing SPLL (480 MHz)
 * through CLKOUT channel 0 divided by 20 (24 MHz), identical to the
 * reference firmware, without the FreeRTOS-dependent esp_clock_output layer.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/signal.h>
#include <time.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/video.h>
#include <nuttx/video/v4l2_cap.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <arch/board/board.h>
#include <arch/chip/gpio_sig_map.h>
#include <metalio/metalio.h>

#include "espressif/esp_gpio.h"

#ifdef CONFIG_ESPRESSIF_MIPI_CSI
#  include "espressif/esp_mipi_csi.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define OV2710_I2C_ADDR        BOARD_I2C_ADDR_OV2710  /* 0x36 (7-bit) */
#define OV2710_SCCB_FREQ       100000                 /* 100 kHz, shared bus */

/* OV2710 product ID: 0x300A = 0x27, 0x300B = 0x10 -> 0x2710. */

#define OV2710_REG_PID_H       0x300a
#define OV2710_REG_PID_L       0x300b
#define OV2710_PID_H           0x27
#define OV2710_PID_L           0x10

/* Default frame: RAW10 1920x1080 @ 25 fps (MIPI 1 lane / 800 Mbps). */

#define OV2710_WIDTH           1920
#define OV2710_HEIGHT          1080

/* CAM_PWDN (TCA9555 P0.2) is active-low: release to power the sensor on. */

#define CAM_POWER_SETTLE_MS    200

/* XCLK: 24 MHz master clock on BOARD_CAM_XCLK_GPIO, derived from the SPLL
 * (480 MHz) through CLKOUT channel 0 divided by 20 — identical to the
 * reference firmware's ESP_CLOCK_ROUTER setup (CLKOUT_SIG_SPLL / 20). */

#define CAM_XCLK_FREQ_HZ       24000000
#define CAM_XCLK_SRC_HZ        480000000
#define CAM_XCLK_DIV           (CAM_XCLK_SRC_HZ / CAM_XCLK_FREQ_HZ) /* 20 */
#define CAM_XCLK_SETTLE_MS     50

/* HP_SYS_CLKRST register block (DR_REG_HP_SYS_CLKRST_BASE = 0x500e6000).
 * These match clk_ll_bind_output_channel()/clk_ll_set_output_channel_divider()
 * /clk_ll_enable_output_channel() for CLKOUT_CHANNEL_1 (DBG_CH0). */

#define HP_SYS_CLKRST_BASE           0x500e6000
#define HP_SYS_CLKRST_DBG_CLK_CTRL0  (HP_SYS_CLKRST_BASE + 0xe4)
#define HP_SYS_CLKRST_DBG_CLK_CTRL1  (HP_SYS_CLKRST_BASE + 0xe8)

#define CLKOUT_SIG_SPLL              1

#define DBG_CH0_SEL_SHIFT            0
#define DBG_CH0_SEL_MASK             (0xffU << DBG_CH0_SEL_SHIFT)
#define DBG_CH0_DIV_SHIFT            24
#define DBG_CH0_DIV_MASK             (0xffU << DBG_CH0_DIV_SHIFT)
#define DBG_CH0_EN_SHIFT             16
#define DBG_CH0_EN_MASK              (1U << DBG_CH0_EN_SHIFT)

/* SPLL (System PLL, 480 MHz) power control.  NuttX's ESP32-P4 clock init
 * only powers CPLL (CPU) and never brings up the PMU, so SPLL — the source
 * behind CLKOUT_SIG_SPLL — stays powered down.  Mirror clk_ll_cpll_enable()
 * and tie SPLL high so the CLKOUT divider below actually produces XCLK. */

#define PMU_IMM_HP_CK_POWER_REG      0x501150cc
#define PMU_TIE_HIGH_XPD_SPLL        (1U << 28)
#define PMU_TIE_HIGH_XPD_SPLL_I2C    (1U << 24)
#define PMU_TIE_HIGH_GLOBAL_SPLL_ICG (1U << 18)

/* GPIO output matrix: confirm DBG_CH0 (CLKOUT ch0) reaches BOARD_CAM_XCLK_GPIO. */

#define GPIO_BASE                    0x500e0000
#define GPIO_FUNC_OUT_SEL_CFG_OFF    0x558

/* Register-list sentinels (mirrors Espressif ov2710_settings.h). */

#define OV2710_REG_DELAY            0xfffe
#define OV2710_REG_END              0xffff

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ov2710_reg_s
{
  uint16_t reg;
  uint8_t  val;
};

struct metalio_camera_s
{
  struct imgdata_s   data;      /* imgdata ops (MIPI-CSI DMA capture) */
  struct imgsensor_s sensor;    /* imgsensor ops (OV2710 SCCB) */
  FAR struct i2c_master_s *i2c; /* Shared I2C0 bus */
  mutex_t lock;                 /* Serialize power / stream state */

  bool available;               /* Sensor detected during init probe */
  bool powered;                 /* PWDN released */
  bool xclk_on;                 /* XCLK running */
  bool exposure_valid;          /* Warm-start seed only; auto AE/AGC stays on */
  bool streaming;
  bool registered;              /* V4L2 node exists; never re-probe a live sensor */
  uint8_t exposure[3];
  uint8_t gain[2];

#ifdef CONFIG_ESPRESSIF_MIPI_CSI
  imgdata_capture_t capture_cb; /* complete_capture() from V4L2 upper-half */
  FAR void *capture_arg;        /* Argument for capture_cb */
  size_t frame_bytes;           /* h*v*10/8 (RAW10 packed) */
#endif
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_MIPI_CSI

/* OV2710 1080p25 init sequence (Espressif ov2710_settings.h). */

static const struct ov2710_reg_s g_ov2710_init_1080p[] =
{
  {0x3008, 0x82}, /* reset */
  {OV2710_REG_DELAY, 0x04},
  {0x4800, 0x01}, /* ensure streaming off / clock lane LP-11 */
  {0x3008, 0x42}, /* sleep en */
  {OV2710_REG_DELAY, 0x04},
  {0x4201, 0x00},
  {0x4202, 0x0f},
  {0x3103, 0x93}, /* PLL clock select */
  {0x3017, 0x7f},
  {0x3018, 0xfc},
  {0x3706, 0x61},
  {0x3712, 0x0c},
  {0x3630, 0x6d},
  {0x3800, 0x01},
  {0x3801, 0xb4},
  {0x3802, 0x00},
  {0x3803, 0x0a},
  {0x3818, 0x80},
  {0x3804, 0x07},
  {0x3805, 0x80},
  {0x3806, 0x04},
  {0x3807, 0x38},
  {0x3808, 0x07},
  {0x3809, 0x80},
  {0x380a, 0x04},
  {0x380b, 0x38},
  {0x3810, 0x10},
  {0x3811, 0x06},
  {0x3812, 0x00},
  {0x3813, 0x00},
  {0x3621, 0x04},
  {0x3604, 0x60},
  {0x3603, 0xa7},
  {0x3631, 0x26},
  {0x3600, 0x04},
  {0x3620, 0x37},
  {0x3623, 0x00},
  {0x3702, 0x9e},
  {0x3703, 0x5c},
  {0x3704, 0x40},
  {0x370d, 0x0f},
  {0x3713, 0x9f},
  {0x3714, 0x4c},
  {0x3710, 0x9e},
  {0x3801, 0xc4},
  {0x3605, 0x05},
  {0x3606, 0x3f},
  {0x302d, 0x90},
  {0x370b, 0x40},
  {0x3716, 0x31},
  {0x3707, 0x52},
  {0x380d, 0x74},
  {0x5181, 0x20},
  {0x518f, 0x00},
  {0x4301, 0xff},
  {0x4303, 0x00},
  {0x3a00, 0x78},
  {0x300f, 0x88},
  {0x3011, 0x28},
  {0x3a1a, 0x06},
  {0x3a18, 0x00},
  {0x3a19, 0x7a},
  {0x3a13, 0x54},
  {0x382e, 0x0f},
  {0x381a, 0x1a},
  {0x401d, 0x02},
  {0x5688, 0x03},
  {0x5684, 0x07},
  {0x5685, 0xa0},
  {0x5686, 0x04},
  {0x5687, 0x43},
  {0x3011, 0x0a},
  {0x300f, 0x8a},
  {0x3017, 0x00},
  {0x3018, 0x00},
  {0x300e, 0x04},
  {0x4801, 0x0f},
  {0x300f, 0xc3},
  {0x3a0f, 0x40},
  {0x3a10, 0x38},
  {0x3a1b, 0x48},
  {0x3a1e, 0x30},
  {0x3a11, 0x90},
  {0x3a1f, 0x10},
  {0x380c, 0x09}, /* HTS H */
  {0x380d, 0x74}, /* HTS L */
  {0x380e, 0x05}, /* VTS H */
  {0x380f, 0x2a}, /* VTS L */
  {0x4800, 0x24},
  {0x503d, 0x00}, /* disable color-bar test pattern */
  {OV2710_REG_DELAY, 0x02},
  {OV2710_REG_END, 0x00},
};

static const struct v4l2_fmtdesc g_ov2710_fmtdescs[] =
{
  {
    .index       = 0,
    .type        = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .flags       = 0,
    .description = "10-bit Bayer BGGR",
    .pixelformat = V4L2_PIX_FMT_SBGGR10,
    .mbus_code   = 0,
  },
};

static const struct v4l2_frmsizeenum g_ov2710_frmsizes[] =
{
  {
    .index        = 0,
    .buf_type     = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_SBGGR10,
    .type         = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete     = { OV2710_WIDTH, OV2710_HEIGHT },
  },
};

static const struct v4l2_frmivalenum g_ov2710_frmintervals[] =
{
  {
    .index        = 0,
    .buf_type     = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_SBGGR10,
    .width        = OV2710_WIDTH,
    .height       = OV2710_HEIGHT,
    .type         = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete     = { 1, 25 },
  },
};

#endif /* CONFIG_ESPRESSIF_MIPI_CSI */

static struct metalio_camera_s g_metalio_camera =
{
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ov2710_sccb_read(FAR struct i2c_master_s *i2c, uint16_t reg,
                            FAR uint8_t *buf, uint8_t len);
static int ov2710_i2c_probe(FAR struct i2c_master_s *i2c);
static void metalio_camera_xclk_start(void);
static void metalio_camera_xclk_stop(void);
static int camera_power_on(FAR struct metalio_camera_s *priv);
static void camera_power_off(FAR struct metalio_camera_s *priv);

#ifdef CONFIG_ESPRESSIF_MIPI_CSI
static int ov2710_sccb_write(FAR struct i2c_master_s *i2c,
                             uint16_t reg, uint8_t val);
static int ov2710_set_reg_bits(FAR struct i2c_master_s *i2c, uint16_t reg,
                               uint8_t offset, uint8_t length, uint8_t value);
static int ov2710_disable_test_pattern(FAR struct metalio_camera_s *priv);
static int ov2710_write_array(FAR struct i2c_master_s *i2c,
                              FAR const struct ov2710_reg_s *regs);
static int ov2710_set_stream(FAR struct metalio_camera_s *priv, bool enable);

static bool ov2710_is_available(FAR struct imgsensor_s *sensor);
static int ov2710_init(FAR struct imgsensor_s *sensor);
static int ov2710_uninit(FAR struct imgsensor_s *sensor);
static FAR const char *ov2710_get_driver_name(FAR struct imgsensor_s *sensor);
static int ov2710_validate_frame_setting(FAR struct imgsensor_s *sensor,
                                         imgsensor_stream_type_t type,
                                         uint8_t nr_datafmts,
                                         FAR imgsensor_format_t *datafmts,
                                         FAR imgsensor_interval_t *interval);
static int ov2710_start_capture(FAR struct imgsensor_s *sensor,
                                imgsensor_stream_type_t type,
                                uint8_t nr_datafmts,
                                FAR imgsensor_format_t *datafmts,
                                FAR imgsensor_interval_t *interval);
static int ov2710_stop_capture(FAR struct imgsensor_s *sensor,
                               imgsensor_stream_type_t type);
static int ov2710_get_frame_interval(FAR struct imgsensor_s *sensor,
                                     imgsensor_stream_type_t type,
                                     FAR imgsensor_interval_t *interval);

static int ov2710_data_init(FAR struct imgdata_s *data);
static int ov2710_data_uninit(FAR struct imgdata_s *data);
static int ov2710_data_set_buf(FAR struct imgdata_s *data,
                               uint8_t nr_datafmts,
                               FAR imgdata_format_t *datafmts,
                               uint8_t *addr, uint32_t size);
static int ov2710_data_validate_frame_setting(FAR struct imgdata_s *data,
                                              uint8_t nr_datafmts,
                                              FAR imgdata_format_t *datafmts,
                                              FAR imgdata_interval_t *interval);
static int ov2710_data_start_capture(FAR struct imgdata_s *data,
                                     uint8_t nr_datafmts,
                                     FAR imgdata_format_t *datafmts,
                                     FAR imgdata_interval_t *interval,
                                     FAR imgdata_capture_t callback,
                                     FAR void *arg);
static int ov2710_data_stop_capture(FAR struct imgdata_s *data);
static void *ov2710_data_alloc(FAR struct imgdata_s *data,
                               uint32_t align_size, uint32_t size);
static void ov2710_data_free(FAR struct imgdata_s *data, void *addr);
static void ov2710_data_frame_done(FAR void *buf, size_t len, FAR void *arg);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Camera initialization runs in task context. Preserve electrical settling
 * time without busy-waiting through display refresh and audio deadlines. */
static void camera_settle_ms(unsigned int ms)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t until = (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000 + ms * 1000;
  for (;;)
    {
      clock_gettime(CLOCK_MONOTONIC, &now);
      int64_t left = until - ((int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000);
      if (left <= 0) break;
      nxsig_usleep((useconds_t)left);
    }
}

static int ov2710_sccb_read(FAR struct i2c_master_s *i2c, uint16_t reg,
                            FAR uint8_t *buf, uint8_t len)
{
  struct i2c_msg_s msgs[2];
  uint8_t regb[2];

  regb[0] = (uint8_t)(reg >> 8);
  regb[1] = (uint8_t)(reg & 0xff);

  msgs[0].frequency = OV2710_SCCB_FREQ;
  msgs[0].addr      = OV2710_I2C_ADDR;
  msgs[0].flags     = 0;
  msgs[0].buffer    = regb;
  msgs[0].length    = 2;

  msgs[1].frequency = OV2710_SCCB_FREQ;
  msgs[1].addr      = OV2710_I2C_ADDR;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = buf;
  msgs[1].length    = len;

  return I2C_TRANSFER(i2c, msgs, 2);
}

#ifdef CONFIG_ESPRESSIF_MIPI_CSI

static int ov2710_sccb_write(FAR struct i2c_master_s *i2c,
                             uint16_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[3];

  buf[0] = (uint8_t)(reg >> 8);
  buf[1] = (uint8_t)(reg & 0xff);
  buf[2] = val;

  msg.frequency = OV2710_SCCB_FREQ;
  msg.addr      = OV2710_I2C_ADDR;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 3;

  return I2C_TRANSFER(i2c, &msg, 1);
}

static int ov2710_set_reg_bits(FAR struct i2c_master_s *i2c, uint16_t reg,
                               uint8_t offset, uint8_t length, uint8_t value)
{
  uint8_t reg_data = 0;
  uint8_t mask;
  int ret;

  ret = ov2710_sccb_read(i2c, reg, &reg_data, 1);
  if (ret < 0)
    {
      return ret;
    }

  mask     = (uint8_t)(((1U << length) - 1U) << offset);
  reg_data = (uint8_t)((reg_data & ~mask) | ((value << offset) & mask));
  return ov2710_sccb_write(i2c, reg, reg_data);
}

/* Match the sensor driver's read/modify/write of the test-pattern bit.
 * This is called with streaming stopped; no group launch is necessary. */

static int ov2710_disable_test_pattern(FAR struct metalio_camera_s *priv)
{
  FAR struct i2c_master_s *i2c = priv->i2c;
  uint8_t tp    = 0;
  int ret = ov2710_set_reg_bits(i2c, 0x503d, 7, 1, 0);
  if (ret < 0) return ret;
  ret = ov2710_sccb_read(i2c, 0x503d, &tp, 1);
  if (ret < 0) return ret;

  if (tp & 0x80)
    {
      return -EIO;
    }

  return ret;
}

static int ov2710_write_array(FAR struct i2c_master_s *i2c,
                              FAR const struct ov2710_reg_s *regs)
{
  int ret = OK;

  while (regs->reg != OV2710_REG_END)
    {
      if (regs->reg == OV2710_REG_DELAY)
        {
          camera_settle_ms(regs->val);
        }
      else
        {
          ret = ov2710_sccb_write(i2c, regs->reg, regs->val);
          if (ret < 0)
            {
              return ret;
            }
        }

      regs++;
    }

  return ret;
}

/* Stream on/off sequence (Espressif ov2710_set_stream()). */

static int ov2710_set_stream(FAR struct metalio_camera_s *priv, bool enable)
{
  int ret;

  if (enable)
    {
      /* The reset default is only two exposure lines, visibly dark while
       * auto exposure converges. Seed from the last real session (or a
       * bounded 512-line cold value), then leave automatic AE/AGC enabled.
       * Dimensions, PLL, frame interval and image quality are unchanged. */
      uint8_t control = 0;
      if (ov2710_sccb_read(priv->i2c, 0x3503, &control, 1) == OK &&
          (control & 3) == 0)
        {
          const uint8_t cold[3] = {0x00, 0x20, 0x00};
          const uint8_t *expo = priv->exposure_valid ? priv->exposure : cold;
          /* AEC is idle with the sensor stopped. Keep its enable bits
           * untouched even if any seed transaction fails. */
          for (unsigned int i = 0; i < 3; ++i)
            ov2710_sccb_write(priv->i2c, 0x3500 + i, expo[i]);
          if (priv->exposure_valid)
            for (unsigned int i = 0; i < 2; ++i)
              ov2710_sccb_write(priv->i2c, 0x350a + i, priv->gain[i]);
        }
      /* Match the HiOS OV2710 start sequence: clear the factory pattern
       * while the sensor is stopped, then start streaming once.  Rewriting
       * 0x503d after 0x3008=0x02 pauses and restarts the sensor immediately
       * after CSI STREAMON, which can leave the receiver waiting for a new
       * frame boundary indefinitely. */
      ret = ov2710_disable_test_pattern(priv);
      if (ret == OK)
        {
          ret = ov2710_sccb_write(priv->i2c, 0x4201, 0x00);
        }

      if (ret == OK)
        {
          ret = ov2710_sccb_write(priv->i2c, 0x4202, 0x00);
        }

      if (ret == OK)
        {
          ret = ov2710_sccb_write(priv->i2c, 0x3008, 0x02);
        }

      if (ret == OK)
        {
          camera_settle_ms(30);
        }

      if (ret == OK)
        {
          uint8_t power = 0, output = 0, mipi = 0, pll = 0;
          int verify = ov2710_sccb_read(priv->i2c, 0x3008, &power, 1);
          verify |= ov2710_sccb_read(priv->i2c, 0x4202, &output, 1);
          verify |= ov2710_sccb_read(priv->i2c, 0x4800, &mipi, 1);
          verify |= ov2710_sccb_read(priv->i2c, 0x300f, &pll, 1);
          syslog(LOG_INFO, "CAM: stream registers power=%02x output=%02x mipi=%02x pll=%02x read=%d\n",
                 power, output, mipi, pll, verify);
          uint8_t phy = 0, divider = 0, multiplier = 0, source = 0;
          verify |= ov2710_sccb_read(priv->i2c, 0x300e, &phy, 1);
          verify |= ov2710_sccb_read(priv->i2c, 0x3010, &divider, 1);
          verify |= ov2710_sccb_read(priv->i2c, 0x3011, &multiplier, 1);
          verify |= ov2710_sccb_read(priv->i2c, 0x3103, &source, 1);
          syslog(LOG_INFO, "CAM: PHY=%02x divider=%02x multiplier=%02x source=%02x read=%d\n",
                 phy, divider, multiplier, source, verify);
          if (verify < 0) ret = verify;
          else if ((power & 0x40) || output != 0) ret = -EIO;
        }
      priv->streaming = ret == OK;
      syslog(LOG_INFO, "CAM: sensor stream on ret=%d\n", ret);
    }
  else
    {
      if (priv->powered && priv->xclk_on && priv->streaming)
        {
          uint8_t expo[3] = {0}, gain[2] = {0}, check[2] = {0};
          int sampled = OK;
          /* OV2710 SCCB uses individually addressed 8-bit registers, as
           * do its chip-ID and upstream exposure/gain reads. A burst read
           * can repeat one byte: seeding that as exposure/gain makes the
           * next preview start very dark or clipped white. */
          for (unsigned int i = 0; i < 3 && sampled == OK; ++i)
            sampled = ov2710_sccb_read(priv->i2c, 0x3500 + i, &expo[i], 1);
          for (unsigned int i = 0; i < 2 && sampled == OK; ++i)
            sampled = ov2710_sccb_read(priv->i2c, 0x350a + i, &gain[i], 1);
          for (unsigned int i = 0; i < 2 && sampled == OK; ++i)
            sampled = ov2710_sccb_read(priv->i2c, 0x3500 + i, &check[i], 1);
          priv->exposure_valid = false;
          if (sampled == OK && memcmp(check, expo, sizeof(check)) == 0 &&
              (expo[0] & 0xf0) == 0 && (gain[0] & 0xfe) == 0 &&
              (expo[0] || expo[1] || (expo[2] & 0xf0)))
            {
              memcpy(priv->exposure, expo, sizeof(expo));
              memcpy(priv->gain, gain, sizeof(gain));
              priv->exposure_valid = true;
            }
        }
      priv->streaming = false;
      ret = ov2710_sccb_write(priv->i2c, 0x3008, 0x42);
      if (ret == OK)
        {
          ret = ov2710_sccb_write(priv->i2c, 0x4201, 0x00);
        }

      if (ret == OK)
        {
          ret = ov2710_sccb_write(priv->i2c, 0x4202, 0x0f);
        }

      if (ret == OK)
        {
          ret = ov2710_disable_test_pattern(priv);
        }
    }

  return ret;
}

#endif /* CONFIG_ESPRESSIF_MIPI_CSI */

/* Single-byte write as an address probe — a zero-length write is unreliable
 * on the ESP32-P4 I2C controller. */

static int ov2710_i2c_probe(FAR struct i2c_master_s *i2c)
{
  struct i2c_msg_s msg;
  uint8_t dummy = 0;

  msg.frequency = OV2710_SCCB_FREQ;
  msg.addr      = OV2710_I2C_ADDR;
  msg.flags     = 0;
  msg.buffer    = &dummy;
  msg.length    = 1;

  return I2C_TRANSFER(i2c, &msg, 1);
}

/* CLKOUT register helpers — MMIO access mirroring the IDF HAL. */

static uint32_t metalio_clk_reg_read(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

static void metalio_clk_reg_write(uintptr_t addr, uint32_t val)
{
  *(volatile uint32_t *)addr = val;
}

/* Drive 24 MHz XCLK onto BOARD_CAM_XCLK_GPIO using CLKOUT channel 0. */

static void metalio_camera_xclk_start(void)
{
  uint32_t v;
  uint32_t gpio_sel;
  uintptr_t gpio_sel_addr;

  /* Power up SPLL.  NuttX's clock init only enables CPLL (CPU); SPLL, the
   * 480 MHz root behind CLKOUT_SIG_SPLL, is left powered down and there is no
   * clk_ll_spll_enable() in the HAL (ESP-IDF brings it up via pmu_init()).
   * Tie it high here so the CLKOUT divider below produces a real 24 MHz XCLK. */

  /* These PMU fields are write-trigger bits, not persistent R/W state.
   * Match the ESP32-P4 HAL's PLL bring-up ordering: assert the PLL and I2C
   * power controls first, then enable the global clock gate separately. */
  metalio_clk_reg_write(PMU_IMM_HP_CK_POWER_REG,
                        PMU_TIE_HIGH_XPD_SPLL |
                        PMU_TIE_HIGH_XPD_SPLL_I2C);
  metalio_clk_reg_write(PMU_IMM_HP_CK_POWER_REG,
                        PMU_TIE_HIGH_GLOBAL_SPLL_ICG);

  /* Select the GPIO function on the pad (IO_MUX) and enable the output
   * driver BEFORE routing the CLKOUT signal through the GPIO matrix.  Without
   * esp_configgpio(OUTPUT), the IO_MUX function select is left at its reset
   * value and the matrix output never reaches the pad, so the sensor sees no
   * XCLK and produces no MIPI clock/data. */

  esp_configgpio(BOARD_CAM_XCLK_GPIO, OUTPUT);
  esp_gpio_matrix_out(BOARD_CAM_XCLK_GPIO, DBG_CH0_CLK_IDX, false, false);

  v = metalio_clk_reg_read(HP_SYS_CLKRST_DBG_CLK_CTRL0);
  v &= ~DBG_CH0_SEL_MASK;
  v |= (uint32_t)CLKOUT_SIG_SPLL << DBG_CH0_SEL_SHIFT;
  v &= ~DBG_CH0_DIV_MASK;
  v |= (uint32_t)(CAM_XCLK_DIV - 1) << DBG_CH0_DIV_SHIFT;
  metalio_clk_reg_write(HP_SYS_CLKRST_DBG_CLK_CTRL0, v);

  v = metalio_clk_reg_read(HP_SYS_CLKRST_DBG_CLK_CTRL1);
  v |= DBG_CH0_EN_MASK;
  metalio_clk_reg_write(HP_SYS_CLKRST_DBG_CLK_CTRL1, v);

  /* Readback for bring-up: confirm SPLL power + CLKOUT sel/div/en + the GPIO
   * matrix out_sel for BOARD_CAM_XCLK_GPIO all took effect. */

  gpio_sel_addr = GPIO_BASE + GPIO_FUNC_OUT_SEL_CFG_OFF +
                  (BOARD_CAM_XCLK_GPIO * 4);
  gpio_sel = metalio_clk_reg_read(gpio_sel_addr);
  syslog(LOG_INFO, "XCLK clock regs: pmu=%08" PRIx32 " dbg0=%08" PRIx32
         " dbg1=%08" PRIx32 "\n",
         metalio_clk_reg_read(PMU_IMM_HP_CK_POWER_REG),
         metalio_clk_reg_read(HP_SYS_CLKRST_DBG_CLK_CTRL0),
         metalio_clk_reg_read(HP_SYS_CLKRST_DBG_CLK_CTRL1));
  syslog(LOG_INFO, "XCLK GPIO%u output-select @%08" PRIxPTR " = %08" PRIx32
         " (expected signal %u)\n",
         (unsigned int)BOARD_CAM_XCLK_GPIO, gpio_sel_addr, gpio_sel,
         (unsigned int)DBG_CH0_CLK_IDX);
}

static void metalio_camera_xclk_stop(void)
{
  uint32_t v;

  v = metalio_clk_reg_read(HP_SYS_CLKRST_DBG_CLK_CTRL1);
  v &= ~DBG_CH0_EN_MASK;
  metalio_clk_reg_write(HP_SYS_CLKRST_DBG_CLK_CTRL1, v);

  /* Return the pin to a plain GPIO output. */

  esp_gpio_matrix_out(BOARD_CAM_XCLK_GPIO, SIG_GPIO_OUT_IDX, false, false);
}

static int camera_power_on(FAR struct metalio_camera_s *priv)
{
  int ret;

  if (!priv->powered)
    {
      /* PWDN active-low: release to power the sensor on. */

      ret = metalio_tca9555_write_pin(BOARD_IOEXP_CAM_PWDN, false);
      if (ret < 0)
        {
          return ret;
        }

      camera_settle_ms(CAM_POWER_SETTLE_MS);
      priv->powered = true;
    }

  if (!priv->xclk_on)
    {
      metalio_camera_xclk_start();
      camera_settle_ms(CAM_XCLK_SETTLE_MS);
      priv->xclk_on = true;
    }

  return OK;
}

static void camera_power_off(FAR struct metalio_camera_s *priv)
{
  if (priv->xclk_on)
    {
      metalio_camera_xclk_stop();
      priv->xclk_on = false;
    }

  if (priv->powered)
    {
      metalio_tca9555_write_pin(BOARD_IOEXP_CAM_PWDN, true);
      priv->powered = false;
    }
}

/****************************************************************************
 * imgsensor ops (OV2710 SCCB)
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_MIPI_CSI

#define CAMERA_FROM_SENSOR(s) \
  ((FAR struct metalio_camera_s *)((FAR char *)(s) - \
   offsetof(struct metalio_camera_s, sensor)))
#define CAMERA_FROM_DATA(d) \
  ((FAR struct metalio_camera_s *)((FAR char *)(d) - \
   offsetof(struct metalio_camera_s, data)))

static bool ov2710_is_available(FAR struct imgsensor_s *sensor)
{
  FAR struct metalio_camera_s *priv = CAMERA_FROM_SENSOR(sensor);
  return priv->available;
}

static int ov2710_init(FAR struct imgsensor_s *sensor)
{
  FAR struct metalio_camera_s *priv = CAMERA_FROM_SENSOR(sensor);
  int ret;

  nxmutex_lock(&priv->lock);

  ret = camera_power_on(priv);
  if (ret == OK)
    {
      ret = ov2710_write_array(priv->i2c, g_ov2710_init_1080p);
      if (ret == OK)
        {
          /* Leave the sensor in a known stopped state. */

          ret = ov2710_set_stream(priv, false);
          if (ret == OK)
            {
              ret = ov2710_disable_test_pattern(priv);
            }
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

static int ov2710_uninit(FAR struct imgsensor_s *sensor)
{
  FAR struct metalio_camera_s *priv = CAMERA_FROM_SENSOR(sensor);

  nxmutex_lock(&priv->lock);
  camera_power_off(priv);
  nxmutex_unlock(&priv->lock);
  return OK;
}

static FAR const char *ov2710_get_driver_name(FAR struct imgsensor_s *sensor)
{
  return "OV2710";
}

static int ov2710_validate_frame_setting(FAR struct imgsensor_s *sensor,
                                         imgsensor_stream_type_t type,
                                         uint8_t nr_datafmts,
                                         FAR imgsensor_format_t *datafmts,
                                         FAR imgsensor_interval_t *interval)
{
  if (nr_datafmts != 1 || datafmts == NULL)
    {
      return -EINVAL;
    }

  if (datafmts[0].pixelformat != IMGSENSOR_PIX_FMT_SBGGR10)
    {
      return -EINVAL;
    }

  if (datafmts[0].width != OV2710_WIDTH ||
      datafmts[0].height != OV2710_HEIGHT)
    {
      return -EINVAL;
    }

  return OK;
}

static int ov2710_start_capture(FAR struct imgsensor_s *sensor,
                                imgsensor_stream_type_t type,
                                uint8_t nr_datafmts,
                                FAR imgsensor_format_t *datafmts,
                                FAR imgsensor_interval_t *interval)
{
  FAR struct metalio_camera_s *priv = CAMERA_FROM_SENSOR(sensor);
  int ret;

  ret = ov2710_validate_frame_setting(sensor, type, nr_datafmts,
                                      datafmts, interval);
  if (ret != OK)
    {
      return ret;
    }

  nxmutex_lock(&priv->lock);
  ret = ov2710_set_stream(priv, true);
  nxmutex_unlock(&priv->lock);
  return ret;
}

static int ov2710_stop_capture(FAR struct imgsensor_s *sensor,
                               imgsensor_stream_type_t type)
{
  FAR struct metalio_camera_s *priv = CAMERA_FROM_SENSOR(sensor);

  nxmutex_lock(&priv->lock);
  ov2710_set_stream(priv, false);
  nxmutex_unlock(&priv->lock);
  return OK;
}

static int ov2710_get_frame_interval(FAR struct imgsensor_s *sensor,
                                     imgsensor_stream_type_t type,
                                     FAR imgsensor_interval_t *interval)
{
  interval->numerator   = 1;
  interval->denominator = 25;
  return OK;
}

static const struct imgsensor_ops_s g_ov2710_sensor_ops =
{
  .is_available            = ov2710_is_available,
  .init                    = ov2710_init,
  .uninit                  = ov2710_uninit,
  .get_driver_name         = ov2710_get_driver_name,
  .validate_frame_setting  = ov2710_validate_frame_setting,
  .start_capture           = ov2710_start_capture,
  .stop_capture            = ov2710_stop_capture,
  .get_frame_interval      = ov2710_get_frame_interval,
};

/****************************************************************************
 * imgdata ops (ESP32-P4 MIPI-CSI -> DW-GDMA)
 ****************************************************************************/

static int ov2710_data_init(FAR struct imgdata_s *data)
{
  /* V4L2 registers this lower-half before board display bring-up.  Defer CSI
   * host/PHY clock and reset setup until STREAMON so the later DSI panel
   * initialization cannot overwrite the camera PHY state.  The reference
   * HiOS path also creates the video pipeline only after the display is up. */
  (void)data;
  return OK;
}

static int ov2710_data_uninit(FAR struct imgdata_s *data)
{
  esp_mipi_csi_register_frame_done_cb(NULL, NULL);
  esp_mipi_csi_stop();
  return OK;
}

static int ov2710_data_set_buf(FAR struct imgdata_s *data,
                               uint8_t nr_datafmts,
                               FAR imgdata_format_t *datafmts,
                               uint8_t *addr, uint32_t size)
{
  return esp_mipi_csi_enqueue_buffer(addr, size);
}

static int ov2710_data_validate_frame_setting(FAR struct imgdata_s *data,
                                              uint8_t nr_datafmts,
                                              FAR imgdata_format_t *datafmts,
                                              FAR imgdata_interval_t *interval)
{
  if (nr_datafmts != 1 || datafmts == NULL)
    {
      return -EINVAL;
    }

  if (datafmts[0].pixelformat != IMGDATA_PIX_FMT_SBGGR10)
    {
      return -EINVAL;
    }

  return OK;
}

static int ov2710_data_start_capture(FAR struct imgdata_s *data,
                                     uint8_t nr_datafmts,
                                     FAR imgdata_format_t *datafmts,
                                     FAR imgdata_interval_t *interval,
                                     FAR imgdata_capture_t callback,
                                     FAR void *arg)
{
  FAR struct metalio_camera_s *priv = CAMERA_FROM_DATA(data);
  struct esp_mipi_csi_bus_config_s bus =
    {
#ifdef CONFIG_ESPRESSIF_MIPI_CSI_LANES
      .num_data_lanes     = CONFIG_ESPRESSIF_MIPI_CSI_LANES,
#else
      .num_data_lanes     = 1,
#endif
#ifdef CONFIG_ESPRESSIF_MIPI_CSI_LANE_BITRATE_MBPS
      .lane_bit_rate_mbps = CONFIG_ESPRESSIF_MIPI_CSI_LANE_BITRATE_MBPS,
#else
      .lane_bit_rate_mbps = 800,
#endif
    };
  struct esp_mipi_csi_frame_config_s fcfg;
  int ret;

  priv->capture_cb = callback;
  priv->capture_arg = arg;
  priv->frame_bytes = (size_t)datafmts[0].width *
                      datafmts[0].height * 10 / 8;

  ret = esp_mipi_csi_initialize(&bus);
  if (ret != OK)
    {
      return ret;
    }

  fcfg.h_res       = datafmts[0].width;
  fcfg.v_res       = datafmts[0].height;
  fcfg.in_bpp      = 10;
  fcfg.out_bpp     = 10;
  fcfg.byte_swap_en = false;

  ret = esp_mipi_csi_configure(&fcfg);
  if (ret != OK)
    {
      return ret;
    }

  esp_mipi_csi_register_frame_done_cb(ov2710_data_frame_done, priv);

  ret = esp_mipi_csi_start();
  syslog(LOG_INFO, "CAM: csi start ret=%d bytes=%u\n", ret,
         (unsigned)priv->frame_bytes);
  if (ret != OK)
    {
      esp_mipi_csi_register_frame_done_cb(NULL, NULL);
      return ret;
    }

  return OK;
}

static int ov2710_data_stop_capture(FAR struct imgdata_s *data)
{
  FAR struct metalio_camera_s *priv = CAMERA_FROM_DATA(data);

  esp_mipi_csi_stop();
  esp_mipi_csi_register_frame_done_cb(NULL, NULL);
  priv->capture_cb = NULL;
  priv->capture_arg = NULL;
  return OK;
}

static void *ov2710_data_alloc(FAR struct imgdata_s *data,
                               uint32_t align_size, uint32_t size)
{
  /* DMA destination buffers must be cache-line (64-byte) aligned. */

  return kumm_memalign(64, size);
}

static void ov2710_data_free(FAR struct imgdata_s *data, void *addr)
{
  kumm_free(addr);
}

/* Invoked from the DW-GDMA transfer-done ISR.  complete_capture() is designed
 * to run from interrupt context for the continuous (video) capture path. */

static void ov2710_data_frame_done(FAR void *buf, size_t len, FAR void *arg)
{
  FAR struct metalio_camera_s *priv = arg;

  if (priv->capture_cb != NULL)
    {
      static unsigned reported;
      if (reported++ < 3)
        syslog(LOG_INFO, "CAM_FRAME received=%u expected=%u\n",
               (unsigned)len, (unsigned)priv->frame_bytes);
      priv->capture_cb(len == priv->frame_bytes ? 0 : -EIO,
                       (uint32_t)len, NULL, priv->capture_arg);
    }
}

static const struct imgdata_ops_s g_ov2710_data_ops =
{
  .init                   = ov2710_data_init,
  .uninit                 = ov2710_data_uninit,
  .set_buf                = ov2710_data_set_buf,
  .validate_frame_setting = ov2710_data_validate_frame_setting,
  .start_capture          = ov2710_data_start_capture,
  .stop_capture           = ov2710_data_stop_capture,
  .alloc                  = ov2710_data_alloc,
  .free                   = ov2710_data_free,
};

#endif /* CONFIG_ESPRESSIF_MIPI_CSI */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int metalio_camera_power(bool on)
{
  /* CAM_PWDN active-low: on=true -> powered, on=false -> powered down. */

  return metalio_tca9555_write_pin(BOARD_IOEXP_CAM_PWDN, on ? false : true);
}

int metalio_camera_disable_test_pattern(void)
{
  FAR struct metalio_camera_s *priv = &g_metalio_camera;
  int ret;

  if (!priv->available || priv->i2c == NULL)
    {
      return -ENODEV;
    }

  nxmutex_lock(&priv->lock);
  ret = camera_power_on(priv);
  if (ret == OK)
    {
      ret = ov2710_disable_test_pattern(priv);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

int metalio_camera_initialize(FAR struct i2c_master_s *i2c)
{
  FAR struct metalio_camera_s *priv = &g_metalio_camera;
  uint8_t pid_h = 0;
  uint8_t pid_l = 0;
  int ret;

  nxmutex_lock(&priv->lock);
  if (priv->registered)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  if (i2c == NULL)
    {
      i2c = priv->i2c;
    }

  if (i2c == NULL)
    {
      nxmutex_unlock(&priv->lock);
      return -ENODEV;
    }

  priv->i2c = i2c;

  /* Boot-time SCCB self-test: power on -> XCLK on -> chip ID -> power off.
   * This both validates the hardware and caches availability for the V4L2
   * registration below. */

  ret = camera_power_on(priv);
  if (ret == OK)
    {
      ret = ov2710_i2c_probe(i2c);
    }

  if (ret == OK)
    {
      /* Read the two PID registers separately: OV2710 does not reliably
       * auto-increment on SCCB reads, matching the reference firmware's
       * ReadSccbReg16(0x300A) / ReadSccbReg16(0x300B) sequence. */

      ret = ov2710_sccb_read(i2c, OV2710_REG_PID_H, &pid_h, 1);
      if (ret == OK)
        {
          ret = ov2710_sccb_read(i2c, OV2710_REG_PID_L, &pid_l, 1);
        }
    }

  camera_power_off(priv);

  if (ret == OK && pid_h == OV2710_PID_H && pid_l == OV2710_PID_L)
    {
      priv->available = true;
      syslog(LOG_INFO, "OV2710 chip ID: 0x%02x%02x\n", pid_h, pid_l);
    }
  else
    {
      priv->available = false;
      if (ret == OK)
        {
          ret = -ENODEV;
        }
      syslog(LOG_WARNING, "OV2710 not available (ret=%d pid=%02x%02x)\n",
             ret, pid_h, pid_l);
    }

#ifdef CONFIG_ESPRESSIF_MIPI_CSI
  if (priv->available)
    {
      FAR struct imgsensor_s *sensors[1];

      priv->sensor.ops              = &g_ov2710_sensor_ops;
      priv->sensor.fmtdescs         = g_ov2710_fmtdescs;
      priv->sensor.fmtdescs_num     = 1;
      priv->sensor.frmsizes         = g_ov2710_frmsizes;
      priv->sensor.frmsizes_num     = 1;
      priv->sensor.frmintervals     = g_ov2710_frmintervals;
      priv->sensor.frmintervals_num = 1;

      priv->data.ops = &g_ov2710_data_ops;

      sensors[0] = &priv->sensor;
      ret = capture_register("/dev/video0", &priv->data, sensors, 1);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "OV2710 capture_register failed: %d\n", ret);
        }
      else
        {
          priv->registered = true;
          syslog(LOG_INFO, "OV2710 registered at /dev/video0\n");
        }
    }
#endif

  nxmutex_unlock(&priv->lock);
  return ret;
}

/* A transient boot-time SCCB failure must not leave /dev/video0 missing for
 * the entire boot. Reuse the original I2C bus and the camera's own lock;
 * do not reset the shared bus, IO expander, display or audio hardware. */
int metalio_camera_ensure_ready(void)
{
  return metalio_camera_initialize(NULL);
}
