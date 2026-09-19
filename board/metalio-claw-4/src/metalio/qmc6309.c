/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/metalio/qmc6309.c
 *
 * QMC6309 3-axis magnetometer (I2C @ 0x7C).
 *
 * Mirrors the register sequence from MetalioClaw4
 * main/display/screen/test_screen/qmc6309_test.cc:
 *   CHIP_ID 0x00 == 0x90, soft-reset via CR2 0x0B, mode ramp suspend ->
 *   normal -> suspend -> continuous, then read OUTX_L 0x01 (6 bytes).
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <nuttx/arch.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

#define QMC6309_ADDR         0x7c
#define QMC6309_REG_CHIP_ID  0x00
#define QMC6309_REG_OUTX_L   0x01
#define QMC6309_REG_CR1      0x0a
#define QMC6309_REG_CR2      0x0b
#define QMC6309_CHIP_ID      0x90

/* OSR1:1 + RNG0:1 => 0xC0 | 0x10 = 0xD0 base for CR1 */
#define QMC6309_CR1_BASE     0xd0
#define QMC6309_MODE_SUSPEND 0x00
#define QMC6309_MODE_NORMAL  0x01
#define QMC6309_MODE_CONTIN  0x03

#define QMC6309_REG_STATUS   0x09

static FAR struct i2c_master_s *g_i2c;
static uint8_t g_chip_id;
static bool g_chip_id_valid;

static int qmc_write_reg(uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[2] = { reg, val };

  msg.frequency = 100000;
  msg.addr      = QMC6309_ADDR;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  return I2C_TRANSFER(g_i2c, &msg, 1);
}

static int qmc_read_reg(uint8_t reg, FAR uint8_t *val)
{
  struct i2c_msg_s msgs[2];

  msgs[0].frequency = 100000;
  msgs[0].addr      = QMC6309_ADDR;
  msgs[0].flags     = 0;
  msgs[0].buffer    = &reg;
  msgs[0].length    = 1;

  msgs[1].frequency = 100000;
  msgs[1].addr      = QMC6309_ADDR;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = val;
  msgs[1].length    = 1;

  return I2C_TRANSFER(g_i2c, msgs, 2);
}

static int qmc_read_regs(uint8_t reg, FAR uint8_t *buf, int len)
{
  struct i2c_msg_s msgs[2];

  msgs[0].frequency = 100000;
  msgs[0].addr      = QMC6309_ADDR;
  msgs[0].flags     = 0;
  msgs[0].buffer    = &reg;
  msgs[0].length    = 1;

  msgs[1].frequency = 100000;
  msgs[1].addr      = QMC6309_ADDR;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = buf;
  msgs[1].length    = len;

  return I2C_TRANSFER(g_i2c, msgs, 2);
}

static void qmc_set_mode(uint8_t mode)
{
  uint8_t cr1 = (uint8_t)(QMC6309_CR1_BASE | (mode & 0x03));

  (void)qmc_write_reg(QMC6309_REG_CR1, cr1);
  (void)qmc_write_reg(QMC6309_REG_CR2, 0x03);
  up_mdelay(20);
}

int metalio_qmc6309_initialize(FAR struct i2c_master_s *i2c)
{
  uint8_t chip_id = 0;
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  g_i2c = i2c;

  ret = qmc_read_reg(QMC6309_REG_CHIP_ID, &chip_id);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "QMC6309 not present: %d\n", ret);
      return ret;
    }

  if (chip_id != QMC6309_CHIP_ID)
    {
      syslog(LOG_WARNING, "QMC6309 CHIP_ID=0x%02X expect 0x90\n", chip_id);
      return -ENODEV;
    }

  (void)qmc_write_reg(QMC6309_REG_CR2, 0x80);
  up_mdelay(25);
  (void)qmc_write_reg(QMC6309_REG_CR2, 0x00);
  up_mdelay(25);

  qmc_set_mode(QMC6309_MODE_SUSPEND);
  qmc_set_mode(QMC6309_MODE_NORMAL);
  qmc_set_mode(QMC6309_MODE_SUSPEND);
  qmc_set_mode(QMC6309_MODE_CONTIN);
  up_mdelay(50);

  g_chip_id = chip_id;
  g_chip_id_valid = true;
  syslog(LOG_INFO, "QMC6309 ready, CHIP_ID=0x%02X\n", chip_id);
  return OK;
}

int metalio_qmc6309_chip_id(FAR uint8_t *id)
{
  if (!g_chip_id_valid || id == NULL)
    {
      return -ENODEV;
    }

  *id = g_chip_id;
  return OK;
}

int metalio_qmc6309_read_raw_ex(FAR int16_t *mx, FAR int16_t *my,
                                FAR int16_t *mz, FAR uint8_t *status)
{
  uint8_t buf[6];
  uint8_t st = 0;
  int ret;

  if (g_i2c == NULL)
    {
      return -EINVAL;
    }

  if (status != NULL)
    {
      ret = qmc_read_reg(QMC6309_REG_STATUS, &st);
      if (ret < 0)
        {
          return ret;
        }

      *status = st;
    }

  ret = qmc_read_regs(QMC6309_REG_OUTX_L, buf, 6);
  if (ret < 0)
    {
      return ret;
    }

  if (mx != NULL)
    {
      *mx = (int16_t)((buf[1] << 8) | buf[0]);
    }

  if (my != NULL)
    {
      *my = (int16_t)((buf[3] << 8) | buf[2]);
    }

  if (mz != NULL)
    {
      *mz = (int16_t)((buf[5] << 8) | buf[4]);
    }

  return OK;
}

int metalio_qmc6309_read_raw(FAR int16_t *mx, FAR int16_t *my,
                             FAR int16_t *mz)
{
  return metalio_qmc6309_read_raw_ex(mx, my, mz, NULL);
}
