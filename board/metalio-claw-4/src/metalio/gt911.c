/****************************************************************************
 * GT911 touch controller -> /dev/input0
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/kmalloc.h>
#include <nuttx/wqueue.h>
#include <syslog.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

#ifndef CONFIG_INPUT_TOUCHSCREEN

int metalio_gt911_initialize(FAR struct i2c_master_s *i2c)
{
  (void)i2c;
  syslog(LOG_WARNING, "GT911: CONFIG_INPUT_TOUCHSCREEN not enabled\n");
  return -ENOSYS;
}

#else

struct gt911_dev_s
{
  struct touch_lowerhalf_s lower;
  FAR struct i2c_master_s *i2c;
  uint8_t addr;
  struct work_s work;
  bool pressed;          /* True if a finger is currently reported down */
  uint16_t last_x;       /* Last reported X (for TOUCH_UP) */
  uint16_t last_y;       /* Last reported Y (for TOUCH_UP) */
};

static FAR struct gt911_dev_s *g_gt911;

/* GT911 I2C frequency — 200 kHz, matching the original MetalioClaw4
 * firmware (400k caused intermittent failures on shared bus). */
#define GT911_I2C_FREQ  200000

/* Touch-path debug logging.  The original MetalioClaw4 firmware gates its
 * touch logging behind TOUCH_FEED_DEBUG (off in production) and reads the
 * touch chip from a low-priority task, so nothing prints per touch event.
 *
 * On this single-core NuttX build the GT911 poll loop runs on HPWORK at
 * priority 224 while the LVGL render thread runs at ~100; a
 * printf()+fflush(stdout) on every MOVE event blocks on the 115200-baud
 * console UART and repeatedly preempts the render thread, which is what
 * makes swiping drop frames.  Keep this 0 in production. */
#define GT911_DEBUG 0

static int gt911_read_status(FAR struct gt911_dev_s *dev, FAR uint8_t *status)
{
  struct i2c_msg_s msgs[2];
  uint8_t reg[2] = { 0x81, 0x4e };

  msgs[0].frequency = GT911_I2C_FREQ;
  msgs[0].addr = dev->addr;
  msgs[0].flags = 0;
  msgs[0].buffer = reg;
  msgs[0].length = 2;
  msgs[1].frequency = GT911_I2C_FREQ;
  msgs[1].addr = dev->addr;
  msgs[1].flags = I2C_M_READ;
  msgs[1].buffer = status;
  msgs[1].length = 1;
  return I2C_TRANSFER(dev->i2c, msgs, 2);
}

static int gt911_read_point0(FAR struct gt911_dev_s *dev,
                             FAR uint16_t *x, FAR uint16_t *y)
{
  struct i2c_msg_s msgs[2];
  uint8_t reg[2] = { 0x81, 0x50 };
  uint8_t data[8];
  int ret;

  msgs[0].frequency = GT911_I2C_FREQ;
  msgs[0].addr = dev->addr;
  msgs[0].flags = 0;
  msgs[0].buffer = reg;
  msgs[0].length = 2;
  msgs[1].frequency = GT911_I2C_FREQ;
  msgs[1].addr = dev->addr;
  msgs[1].flags = I2C_M_READ;
  msgs[1].buffer = data;
  msgs[1].length = 8;
  ret = I2C_TRANSFER(dev->i2c, msgs, 2);
  if (ret < 0)
    {
      return ret;
    }

  *x = data[0] | ((uint16_t)data[1] << 8);
  *y = data[2] | ((uint16_t)data[3] << 8);
  return OK;
}

/* Write 0 to the GT911 status register (0x814E) to clear the "buffer ready"
 * flag. GT911 will not report a new frame until the host acknowledges the
 * previous one by clearing this bit.
 *
 * IMPORTANT: the ESP32 I2C driver issues START+STOP per i2c_msg_s.  A GT911
 * write is register-address(2) + data(1) in a single transaction, so it must
 * be a single message with a 3-byte buffer — splitting it into two messages
 * (address, then data) produces two separate transactions and the controller
 * never sees the data byte. */
static int gt911_clear_buffer(FAR struct gt911_dev_s *dev)
{
  struct i2c_msg_s msg;
  uint8_t buf[3] = { 0x81, 0x4e, 0x00 };

  msg.frequency = GT911_I2C_FREQ;
  msg.addr = dev->addr;
  msg.flags = 0;
  msg.buffer = buf;
  msg.length = 3;
  return I2C_TRANSFER(dev->i2c, &msg, 1);
}

/* Read `len` bytes starting at 16-bit register address `reg`. */
static int gt911_read_regs(FAR struct gt911_dev_s *dev, uint16_t reg,
                           FAR uint8_t *buf, uint8_t len)
{
  struct i2c_msg_s msgs[2];
  uint8_t regb[2];

  regb[0] = (uint8_t)(reg >> 8);
  regb[1] = (uint8_t)(reg & 0xff);

  msgs[0].frequency = GT911_I2C_FREQ;
  msgs[0].addr = dev->addr;
  msgs[0].flags = 0;
  msgs[0].buffer = regb;
  msgs[0].length = 2;
  msgs[1].frequency = GT911_I2C_FREQ;
  msgs[1].addr = dev->addr;
  msgs[1].flags = I2C_M_READ;
  msgs[1].buffer = buf;
  msgs[1].length = len;
  return I2C_TRANSFER(dev->i2c, msgs, 2);
}

/* Write a single byte to a 16-bit register address.  Same single-message
 * constraint as gt911_clear_buffer() above. */
static int gt911_write_reg(FAR struct gt911_dev_s *dev, uint16_t reg,
                           uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[3];

  buf[0] = (uint8_t)(reg >> 8);
  buf[1] = (uint8_t)(reg & 0xff);
  buf[2] = val;

  msg.frequency = GT911_I2C_FREQ;
  msg.addr = dev->addr;
  msg.flags = 0;
  msg.buffer = buf;
  msg.length = 3;
  return I2C_TRANSFER(dev->i2c, &msg, 1);
}

/* Simple I2C address probe — sends a single data byte (the high byte of a
 * GT911 register address) to check if the device ACKs its slave address.
 * A zero-length write is unreliable on the ESP32-P4 I2C controller (the
 * byte_num==0 command never toggles SDA). */
static int gt911_i2c_probe(FAR struct i2c_master_s *i2c, uint8_t addr)
{
  struct i2c_msg_s msg;
  uint8_t dummy = 0;

  msg.frequency = GT911_I2C_FREQ;
  msg.addr = addr;
  msg.flags = 0;          /* Write */
  msg.buffer = &dummy;
  msg.length = 1;         /* Single-byte write = address probe */
  return I2C_TRANSFER(i2c, &msg, 1);
}

static void gt911_poll_work(FAR void *arg)
{
  FAR struct gt911_dev_s *dev = arg;
#if GT911_DEBUG
  static uint32_t poll_count;
#endif
  uint8_t status = 0;
  struct touch_sample_s sample;
  uint16_t x = 0;
  uint16_t y = 0;

#if GT911_DEBUG
  poll_count++;
#endif

  if (gt911_read_status(dev, &status) < 0)
    {
      /* Bus error — back off slightly and retry; GT911 may still be
       * finishing its post-reset init. */
      work_queue(HPWORK, &dev->work, gt911_poll_work, dev, MSEC2TICK(50));
      return;
    }

  /* Throttled heartbeat: dump raw status roughly once per second so we can
   * tell from the serial log whether GT911 ever raises the buffer-ready
   * flag (0x80) or the touch-count nibble (0x0f). */
#if GT911_DEBUG
  if ((poll_count % 50) == 0)
    {
      printf("GT:status=0x%02x pressed=%d\n", status, dev->pressed);
      fflush(stdout);
    }
#endif

  /* Buffer-ready bit (0x80) is the authoritative "a frame is available"
   * signal; the low nibble holds the number of active points.
   *
   * IMPORTANT: a finger held perfectly still does NOT re-assert the
   * buffer-ready bit (the GT911 only flags new/updated frames).  We must
   * therefore report TOUCH_UP only when the controller *explicitly* flags
   * a frame with zero touch points — never when the bit is merely absent.
   * Treating "no buffer-ready bit" as a lift would split every
   * hold-still-then-resume gesture into a spurious UP followed by a
   * spurious DOWN, which is the "stops then can't keep touching" symptom. */
  if (status & 0x80)
    {
      uint8_t points = status & 0x0f;

      if (points > 0 && points <= 5)
        {
          if (gt911_read_point0(dev, &x, &y) >= 0)
            {
              memset(&sample, 0, sizeof(sample));
              sample.npoints = 1;
              sample.point[0].id = 0;
              sample.point[0].x = x;
              sample.point[0].y = y;

              if (!dev->pressed)
                {
                  sample.point[0].flags = TOUCH_DOWN | TOUCH_ID_VALID |
                                          TOUCH_POS_VALID;
                  dev->pressed = true;
                }
              else if (x != dev->last_x || y != dev->last_y)
                {
                  sample.point[0].flags = TOUCH_MOVE | TOUCH_ID_VALID |
                                          TOUCH_POS_VALID;
                }
              else
                {
                  /* Held in place — no state change to report */
                  sample.npoints = 0;
                }

              dev->last_x = x;
              dev->last_y = y;

              if (sample.npoints > 0)
                {
#if GT911_DEBUG
                  printf("GT:touch x=%u y=%u f=0x%02x\n", x, y,
                         sample.point[0].flags);
                  fflush(stdout);
#endif
                  touch_event(dev->lower.priv, &sample);
                }
            }
        }
      else if (dev->pressed)
        {
          /* Finger lifted — GT911 flags a frame with zero touch points.
           * Report TOUCH_UP with the last known position. */
          memset(&sample, 0, sizeof(sample));
          sample.npoints = 1;
          sample.point[0].id = 0;
          sample.point[0].x = dev->last_x;
          sample.point[0].y = dev->last_y;
          sample.point[0].flags = TOUCH_UP | TOUCH_ID_VALID | TOUCH_POS_VALID;
#if GT911_DEBUG
          printf("GT:touch x=%u y=%u f=0x%02x\n", dev->last_x, dev->last_y,
                 sample.point[0].flags);
          fflush(stdout);
#endif
          touch_event(dev->lower.priv, &sample);
          dev->pressed = false;
        }
    }

  /* Acknowledge the status register (write 0 to 0x814E) on EVERY poll cycle,
   * not just when a frame was present.  The GT911 only stays in its normal
   * scanning mode while the host keeps acknowledging the status register;
   * without this keep-alive it drops into low-power "green" mode after
   * Low_Power_Control seconds of no touch and then stops reporting touches.
   * The ESP-IDF and NuttX esp32s3-box GT911 drivers both do this write
   * unconditionally, which matches the "touch works at first then stops"
   * symptom observed on the board. */
  gt911_clear_buffer(dev);

  /* 40ms poll matches the original MetalioClaw4 touch_feed period; 20ms
   * over-drives the shared I2C bus (GT911 / TCA9555 / BQ27220) and causes
   * the HPWORK thread to preempt the LVGL render thread twice as often. */
  work_queue(HPWORK, &dev->work, gt911_poll_work, dev, MSEC2TICK(40));
}

int metalio_gt911_initialize(FAR struct i2c_master_s *i2c)
{
  FAR struct gt911_dev_s *dev;
  uint8_t addrs[] =
    {
      BOARD_TOUCH_GT911_ADDR_PRIMARY,    /* 0x5d */
      BOARD_TOUCH_GT911_ADDR_SECONDARY,  /* 0x14 */
      0x28,                              /* config-mode address (INT low) */
      0x29                               /* config-mode address (INT low) */
    };
  int naddrs = sizeof(addrs) / sizeof(addrs[0]);
  int i;
  int ret = -ENODEV;
  uint8_t status = 0;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  dev = kmm_zalloc(sizeof(*dev));
  if (dev == NULL)
    {
      return -ENOMEM;
    }

  dev->i2c = i2c;
  dev->lower.maxpoint = 1;

  /* Phase 1: address probe (empty write) — finds which address the
   * GT911 is listening on. This is more reliable than a register read
   * because GT911 may not respond to reads until configured. */

  for (i = 0; i < naddrs; i++)
    {
      ret = gt911_i2c_probe(i2c, addrs[i]);
      if (ret >= 0)
        {
          dev->addr = addrs[i];
          syslog(LOG_INFO, "GT911 probe: address 0x%02x ACKed\n", addrs[i]);
          break;
        }
    }

  if (ret < 0)
    {
      kmm_free(dev);
      syslog(LOG_ERR, "GT911 probe failed (no ACK at 0x%02x/0x%02x/"
             "0x%02x/0x%02x)\n",
             addrs[0], addrs[1], addrs[2], addrs[3]);
      return ret;
    }

  /* Phase 2: confirm the chip is a real GT911 by reading its product ID
   * (0x8140 -> "911" / "911P") and config version (0x8047).  If these fail
   * the address probe matched some other device. */

  {
    uint8_t id[4] = { 0, 0, 0, 0 };
    uint8_t cfg[2] = { 0, 0 };

    if (gt911_read_regs(dev, 0x8140, id, 4) >= 0)
      {
        syslog(LOG_INFO, "GT911 product ID: %c%c%c%c\n",
               id[0], id[1], id[2], id[3]);
      }
    else
      {
        syslog(LOG_WARNING, "GT911 product ID read failed\n");
      }

    if (gt911_read_regs(dev, 0x8047, cfg, 2) >= 0)
      {
        syslog(LOG_INFO, "GT911 config version: 0x%02x%02x\n",
               cfg[0], cfg[1]);
      }
    else
      {
        syslog(LOG_WARNING, "GT911 config version read failed\n");
      }
  }

  /* Phase 3: try to read the status register. If this fails, register
   * the device anyway — the poll loop will keep retrying and the touch
   * may start working once GT911 finishes its internal init. */

  ret = gt911_read_status(dev, &status);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "GT911 status read failed (%d), continuing\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "GT911 status: 0x%02x\n", status);
    }

  /* Dump the config resolution (0x8048..0x804B) so we can confirm the GT911
   * was configured for this 720x720 panel.  X/Y max are 16-bit little-endian
   * values immediately after the 0x8047 config-version byte. */
  {
    uint8_t res[4] = { 0, 0, 0, 0 };
    if (gt911_read_regs(dev, 0x8048, res, 4) >= 0)
      {
        uint16_t x_max = (uint16_t)(res[0] | (res[1] << 8));
        uint16_t y_max = (uint16_t)(res[2] | (res[3] << 8));
        syslog(LOG_INFO, "GT911 resolution: %ux%u\n", x_max, y_max);
      }
  }

  /* Soft reset (0x8040 = 2) then normal mode (0x8040 = 0).  A soft reset
   * re-arms the touch-scanning engine in case the controller came up in a
   * non-scanning state after the hardware reset pulse. */
  gt911_write_reg(dev, 0x8040, 0x02);
  up_mdelay(50);
  if (gt911_write_reg(dev, 0x8040, 0x00) >= 0)
    {
      syslog(LOG_INFO, "GT911 command register set to normal mode\n");
    }

  /* Confirm the controller actually ended up in normal (operation) mode and
   * dump the key config fields that control whether touch scanning is
   * enabled.  If touch_count == 0 or the command register is stuck in a
   * non-zero state, the panel will never report touches. */
  {
    uint8_t cmd = 0xff;
    uint8_t touch_count = 0;
    uint8_t touch_thr = 0;
    uint8_t leave_thr = 0;
    uint8_t refresh = 0;
    uint8_t low_pwr = 0;

    if (gt911_read_regs(dev, 0x8040, &cmd, 1) >= 0)
      {
        syslog(LOG_INFO, "GT911 command register: 0x%02x\n", cmd);
      }

    if (gt911_read_regs(dev, 0x804c, &touch_count, 1) >= 0)
      {
        syslog(LOG_INFO, "GT911 touch number: %u\n", touch_count);
      }

    if (gt911_read_regs(dev, 0x8053, &touch_thr, 1) >= 0 &&
        gt911_read_regs(dev, 0x8054, &leave_thr, 1) >= 0 &&
        gt911_read_regs(dev, 0x8056, &refresh, 1) >= 0 &&
        gt911_read_regs(dev, 0x8055, &low_pwr, 1) >= 0)
      {
        syslog(LOG_INFO, "GT911 cfg thr=%u leave=%u refresh=%u lp=%u\n",
               touch_thr, leave_thr, refresh, low_pwr);
      }
  }

  ret = touch_register(&dev->lower, "/dev/input0", 1);
  if (ret < 0)
    {
      kmm_free(dev);
      return ret;
    }

  g_gt911 = dev;
  /* Defer the first poll until after board bring-up finishes GPS / BT /
   * TCA9555 / SDIO. Starting the 40ms I2C poll immediately races the
   * shared bus and can stall the init thread right after this log line. */
  work_queue(HPWORK, &dev->work, gt911_poll_work, dev, MSEC2TICK(3000));
  syslog(LOG_INFO, "GT911 @0x%02x -> /dev/input0\n", dev->addr);
  return OK;
}

#endif /* CONFIG_INPUT_TOUCHSCREEN */
