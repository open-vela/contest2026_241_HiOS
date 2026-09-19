/****************************************************************************
 * NU1680 Qi wireless charger receiver
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>
#include <syslog.h>
#include <errno.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

static FAR struct i2c_master_s *g_i2c;

static int nu_write(FAR struct i2c_master_s *i2c, uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[2] = { reg, val };
  msg.frequency = 100000;
  msg.addr = BOARD_I2C_ADDR_NU1680;
  msg.flags = 0;
  msg.buffer = buf;
  msg.length = 2;
  return I2C_TRANSFER(i2c, &msg, 1);
}

static int nu_read(FAR struct i2c_master_s *i2c, uint8_t reg, FAR uint8_t *val)
{
  struct i2c_msg_s msgs[2];
  msgs[0].frequency = 100000;
  msgs[0].addr = BOARD_I2C_ADDR_NU1680;
  msgs[0].flags = 0;
  msgs[0].buffer = &reg;
  msgs[0].length = 1;
  msgs[1].frequency = 100000;
  msgs[1].addr = BOARD_I2C_ADDR_NU1680;
  msgs[1].flags = I2C_M_READ;
  msgs[1].buffer = val;
  msgs[1].length = 1;
  return I2C_TRANSFER(i2c, msgs, 2);
}

int metalio_nu1680_initialize(FAR struct i2c_master_s *i2c)
{
  uint8_t cur = 0;
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  g_i2c = i2c;

  ret = nu_read(i2c, 0x1e, &cur);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "NU1680 not present: %d\n", ret);
      return ret;
    }

  /* Keep high 5 bits, set ILIM [2:0] = 000 -> 1.4A */

  ret = nu_write(i2c, 0x1e, (uint8_t)(cur & 0xf8));
  if (ret < 0)
    {
      return ret;
    }

  /* Disable temperature protection (reg 0x15 = 0) */

  ret = nu_write(i2c, 0x15, 0x00);
  syslog(LOG_INFO, "NU1680 ready, ILIM=1.4A\n");
  return ret;
}

int metalio_nu1680_probe(void)
{
  uint8_t cur = 0;

  if (g_i2c == NULL)
    {
      return -ENODEV;
    }

  return nu_read(g_i2c, 0x1e, &cur);
}
