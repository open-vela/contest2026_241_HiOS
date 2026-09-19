/****************************************************************************
 * TCA9555 IO expander driver for Metalio Claw4
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/kmalloc.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

struct tca9555_dev_s
{
  FAR struct i2c_master_s *i2c;
  uint8_t addr;
  uint16_t out;
  uint16_t dir; /* 1 = input */
};

static FAR struct tca9555_dev_s *g_tca;

static int tca_write_regs(FAR struct tca9555_dev_s *dev, uint8_t reg,
                          uint16_t value)
{
  struct i2c_msg_s msg;
  uint8_t buf[3];
  buf[0] = reg;
  buf[1] = value & 0xff;
  buf[2] = (value >> 8) & 0xff;
  msg.frequency = 400000;
  msg.addr = dev->addr;
  msg.flags = 0;
  msg.buffer = buf;
  msg.length = 3;
  return I2C_TRANSFER(dev->i2c, &msg, 1);
}

static int tca_read_regs(FAR struct tca9555_dev_s *dev, uint8_t reg,
                         FAR uint16_t *value)
{
  struct i2c_msg_s msgs[2];
  uint8_t buf[2];
  int ret;

  msgs[0].frequency = 400000;
  msgs[0].addr = dev->addr;
  msgs[0].flags = 0;
  msgs[0].buffer = &reg;
  msgs[0].length = 1;

  msgs[1].frequency = 400000;
  msgs[1].addr = dev->addr;
  msgs[1].flags = I2C_M_READ;
  msgs[1].buffer = buf;
  msgs[1].length = 2;

  ret = I2C_TRANSFER(dev->i2c, msgs, 2);
  if (ret >= 0)
    {
      *value = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }

  return ret;
}

int metalio_tca9555_initialize(FAR struct i2c_master_s *i2c)
{
  FAR struct tca9555_dev_s *dev;
  uint16_t dir;
  uint16_t out;
  int ret;

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
  dev->addr = BOARD_TCA9555_ADDR;

  /* Inputs: PWR_KEY, ACCEL_INT, USB_INSERT, WIRELESS_CHG */

  dir = (1u << BOARD_IOEXP_PWR_KEY) |
        (1u << BOARD_IOEXP_ACCEL_INT) |
        (1u << BOARD_IOEXP_USB_INSERT) |
        (1u << BOARD_IOEXP_WIRELESS_CHG);
  out = 0;

  /* Default power domains off except keep safe defaults */

  ret = tca_write_regs(dev, 0x06, dir); /* config */
  if (ret < 0)
    {
      syslog(LOG_ERR, "TCA9555: config write failed %d\n", ret);
      kmm_free(dev);
      return ret;
    }

  ret = tca_write_regs(dev, 0x02, out); /* output */
  if (ret < 0)
    {
      syslog(LOG_ERR, "TCA9555: output write failed %d\n", ret);
      kmm_free(dev);
      return ret;
    }

  dev->dir = dir;
  dev->out = out;
  g_tca = dev;
  syslog(LOG_INFO, "TCA9555 @0x%02x ready\n", dev->addr);
  return OK;
}

int metalio_tca9555_write_pin(int pin, bool level)
{
  if (g_tca == NULL || pin < 0 || pin > 15)
    {
      return -ENODEV;
    }

  if (level)
    {
      g_tca->out |= (1u << pin);
    }
  else
    {
      g_tca->out &= ~(1u << pin);
    }

  return tca_write_regs(g_tca, 0x02, g_tca->out);
}

int metalio_tca9555_read_pin(int pin, FAR bool *level)
{
  uint16_t in;
  int ret;

  if (g_tca == NULL || level == NULL || pin < 0 || pin > 15)
    {
      return -EINVAL;
    }

  ret = tca_read_regs(g_tca, 0x00, &in);
  if (ret < 0)
    {
      return ret;
    }

  *level = (in & (1u << pin)) != 0;
  return OK;
}

int metalio_pwr_shutdown_pulse(void)
{
  /* Continuous 100ms high / 100ms low on PWR_KEY_PULSE until power cuts. */

  int i;
  if (g_tca == NULL)
    {
      return -ENODEV;
    }

  syslog(LOG_WARNING, "Metalio: starting power-off pulse train\n");
  for (i = 0; i < 50; i++)
    {
      metalio_tca9555_write_pin(BOARD_IOEXP_PWR_KEY_PULSE, true);
      usleep(100 * 1000);
      metalio_tca9555_write_pin(BOARD_IOEXP_PWR_KEY_PULSE, false);
      usleep(100 * 1000);
    }

  return OK;
}
