/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/metalio/sc7a20h.c
 *
 * SC7A20H 3-axis accelerometer (I2C @ 0x19).
 *
 * Mirrors the register sequence from MetalioClaw4
 * main/display/screen/test_screen/sc7a20h_test.cc:
 *   WHO_AM_I 0x0F, CTRL_REG1 0x20 = 0x57, CTRL_REG4 0x23 = 0x88,
 *   OUTX_L 0x28 with auto-increment (0x80). Output is 12-bit left-justified
 *   (shift >> 4) in units of ~1 mg/LSB at the configured +/-2g range.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

#define SC7A20H_ADDR          0x19
#define SC7A20H_REG_WHO_AM_I  0x0f
#define SC7A20H_REG_CTRL_REG1 0x20
#define SC7A20H_REG_CTRL_REG4 0x23
#define SC7A20H_REG_OUTX_L    0x28
#define SC7A20H_AUTO_INC      0x80

static FAR struct i2c_master_s *g_i2c;
static uint8_t g_whoami;
static bool g_whoami_valid;

static int sc7a20h_write_reg(uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[2] = { reg, val };

  msg.frequency = 100000;
  msg.addr      = SC7A20H_ADDR;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  return I2C_TRANSFER(g_i2c, &msg, 1);
}

static int sc7a20h_read_reg(uint8_t reg, FAR uint8_t *val)
{
  struct i2c_msg_s msgs[2];

  msgs[0].frequency = 100000;
  msgs[0].addr      = SC7A20H_ADDR;
  msgs[0].flags     = 0;
  msgs[0].buffer    = &reg;
  msgs[0].length    = 1;

  msgs[1].frequency = 100000;
  msgs[1].addr      = SC7A20H_ADDR;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = val;
  msgs[1].length    = 1;

  return I2C_TRANSFER(g_i2c, msgs, 2);
}

static int sc7a20h_read_regs(uint8_t reg, FAR uint8_t *buf, int len)
{
  struct i2c_msg_s msgs[2];

  msgs[0].frequency = 100000;
  msgs[0].addr      = SC7A20H_ADDR;
  msgs[0].flags     = 0;
  msgs[0].buffer    = &reg;
  msgs[0].length    = 1;

  msgs[1].frequency = 100000;
  msgs[1].addr      = SC7A20H_ADDR;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = buf;
  msgs[1].length    = len;

  return I2C_TRANSFER(g_i2c, msgs, 2);
}

int metalio_sc7a20h_initialize(FAR struct i2c_master_s *i2c)
{
  uint8_t who = 0;
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  g_i2c = i2c;

  ret = sc7a20h_read_reg(SC7A20H_REG_WHO_AM_I, &who);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "SC7A20H not present: %d\n", ret);
      return ret;
    }

  ret  = sc7a20h_write_reg(SC7A20H_REG_CTRL_REG1, 0x57);
  ret |= sc7a20h_write_reg(SC7A20H_REG_CTRL_REG4, 0x88);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "SC7A20H configure failed: %d\n", ret);
      return ret;
    }

  g_whoami = who;
  g_whoami_valid = true;
  syslog(LOG_INFO, "SC7A20H ready, WHO_AM_I=0x%02X\n", who);
  return OK;
}

int metalio_sc7a20h_whoami(FAR uint8_t *who)
{
  if (!g_whoami_valid || who == NULL)
    {
      return -ENODEV;
    }

  *who = g_whoami;
  return OK;
}

int metalio_sc7a20h_read_mg(FAR int *ax, FAR int *ay, FAR int *az)
{
  uint8_t buf[6];
  int16_t rx, ry, rz;
  int ret;

  if (g_i2c == NULL)
    {
      return -EINVAL;
    }

  ret = sc7a20h_read_regs(SC7A20H_REG_OUTX_L | SC7A20H_AUTO_INC, buf, 6);
  if (ret < 0)
    {
      return ret;
    }

  rx = (int16_t)((buf[1] << 8) | buf[0]);
  ry = (int16_t)((buf[3] << 8) | buf[2]);
  rz = (int16_t)((buf[5] << 8) | buf[4]);

  if (ax != NULL)
    {
      *ax = (int)(rx >> 4);
    }

  if (ay != NULL)
    {
      *ay = (int)(ry >> 4);
    }

  if (az != NULL)
    {
      *az = (int)(rz >> 4);
    }

  return OK;
}
