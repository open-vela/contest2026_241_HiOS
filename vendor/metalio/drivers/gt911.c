/****************************************************************************
 * GT911 touch controller -> /dev/input0
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/kmalloc.h>
#include <nuttx/wqueue.h>
#include <syslog.h>
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
};

static FAR struct gt911_dev_s *g_gt911;

static int gt911_read_status(FAR struct gt911_dev_s *dev, FAR uint8_t *status)
{
  struct i2c_msg_s msgs[2];
  uint8_t reg[2] = { 0x81, 0x4e };

  msgs[0].frequency = 400000;
  msgs[0].addr = dev->addr;
  msgs[0].flags = 0;
  msgs[0].buffer = reg;
  msgs[0].length = 2;
  msgs[1].frequency = 400000;
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

  msgs[0].frequency = 400000;
  msgs[0].addr = dev->addr;
  msgs[0].flags = 0;
  msgs[0].buffer = reg;
  msgs[0].length = 2;
  msgs[1].frequency = 400000;
  msgs[1].addr = dev->addr;
  msgs[1].flags = I2C_M_READ;
  msgs[1].buffer = data;
  msgs[1].length = 8;
  ret = I2C_TRANSFER(dev->i2c, msgs, 2);
  if (ret < 0)
    {
      return ret;
    }

  *x = data[1] | ((uint16_t)data[2] << 8);
  *y = data[3] | ((uint16_t)data[4] << 8);
  return OK;
}

static void gt911_poll_work(FAR void *arg)
{
  FAR struct gt911_dev_s *dev = arg;
  uint8_t status = 0;
  struct touch_sample_s sample;
  uint16_t x = 0;
  uint16_t y = 0;

  if (gt911_read_status(dev, &status) >= 0 && (status & 0x0f) != 0)
    {
      if (gt911_read_point0(dev, &x, &y) >= 0)
        {
          memset(&sample, 0, sizeof(sample));
          sample.npoints = 1;
          sample.point[0].x = x;
          sample.point[0].y = y;
          sample.point[0].flags = TOUCH_DOWN | TOUCH_ID_VALID | TOUCH_POS_VALID;
          touch_event(dev->lower.priv, &sample);
        }
    }

  work_queue(HPWORK, &dev->work, gt911_poll_work, dev, MSEC2TICK(20));
}

int metalio_gt911_initialize(FAR struct i2c_master_s *i2c)
{
  FAR struct gt911_dev_s *dev;
  uint8_t addrs[] =
    {
      BOARD_TOUCH_GT911_ADDR_PRIMARY,
      BOARD_TOUCH_GT911_ADDR_SECONDARY
    };
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

  for (i = 0; i < 2; i++)
    {
      dev->addr = addrs[i];
      ret = gt911_read_status(dev, &status);
      if (ret >= 0)
        {
          break;
        }
    }

  if (ret < 0)
    {
      kmm_free(dev);
      syslog(LOG_ERR, "GT911 probe failed\n");
      return ret;
    }

  ret = touch_register(&dev->lower, "/dev/input0", 1);
  if (ret < 0)
    {
      kmm_free(dev);
      return ret;
    }

  g_gt911 = dev;
  work_queue(HPWORK, &dev->work, gt911_poll_work, dev, MSEC2TICK(20));
  syslog(LOG_INFO, "GT911 @0x%02x -> /dev/input0\n", dev->addr);
  return OK;
}

#endif /* CONFIG_INPUT_TOUCHSCREEN */
