/****************************************************************************
 * TI BQ27220 fuel gauge
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>
#include <syslog.h>
#include <errno.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

static FAR struct i2c_master_s *g_i2c;

static int bq_read16(uint8_t reg, FAR uint16_t *val)
{
  struct i2c_msg_s msgs[2];
  uint8_t buf[2];
  int ret;

  msgs[0].frequency = 100000;
  msgs[0].addr = BOARD_I2C_ADDR_BQ27220;
  msgs[0].flags = 0;
  msgs[0].buffer = &reg;
  msgs[0].length = 1;
  msgs[1].frequency = 100000;
  msgs[1].addr = BOARD_I2C_ADDR_BQ27220;
  msgs[1].flags = I2C_M_READ;
  msgs[1].buffer = buf;
  msgs[1].length = 2;
  ret = I2C_TRANSFER(g_i2c, msgs, 2);
  if (ret >= 0)
    {
      *val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }

  return ret;
}

int metalio_bq27220_initialize(FAR struct i2c_master_s *i2c)
{
  uint16_t volt = 0;
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  g_i2c = i2c;
  ret = bq_read16(0x08, &volt); /* Voltage() */
  if (ret < 0)
    {
      syslog(LOG_ERR, "BQ27220 not responding: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "BQ27220 ready, Vbat=%umV\n", (unsigned)volt);
  return OK;
}

int metalio_bq27220_read_voltage_mv(FAR int *mv)
{
  uint16_t volt = 0;
  int ret;

  if (mv == NULL || g_i2c == NULL)
    {
      return -EINVAL;
    }

  ret = bq_read16(0x08, &volt);
  if (ret < 0)
    {
      return ret;
    }

  *mv = (int)volt;
  return OK;
}

int metalio_bq27220_read_soc(FAR int *percent)
{
  uint16_t soc = 0;
  int ret;

  if (percent == NULL || g_i2c == NULL)
    {
      return -EINVAL;
    }

  ret = bq_read16(0x2c, &soc); /* StateOfCharge() */
  if (ret < 0)
    {
      return ret;
    }

  *percent = (int)(soc & 0xff);
  return OK;
}
