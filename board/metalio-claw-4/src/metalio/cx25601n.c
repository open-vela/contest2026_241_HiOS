/****************************************************************************
 * CX25601N charger (I2C) — NuttX port.
 *
 * Ported from MetalioClaw4 main/boards/common/cx25601n.cc (ESP-IDF).
 * Register map aligned with the MediaTek/CX2560x reference driver.
 *
 * The CX25601N is a standalone 1-cell buck charger present on newer
 * Metalio Claw4 revisions at I2C address 0x6B.  Older boards do not carry
 * it, so initialization is probe-based and non-fatal (mirrors the original
 * firmware: "老设备无此芯片，probe 失败则跳过").
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>

#include <syslog.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <arch/board/board.h>
#include <metalio/metalio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register map */
#define CX25601N_REG_ICHG_LO    0x02
#define CX25601N_REG_ICHG_HI    0x03
#define CX25601N_REG_VREG_LO    0x04
#define CX25601N_REG_VREG_HI    0x05
#define CX25601N_REG_IINDPM_LO  0x06
#define CX25601N_REG_IINDPM_HI  0x07
#define CX25601N_REG_IOTG_LO    0x0a   /* IOTG[3:0] @ [7:4] */
#define CX25601N_REG_IOTG_HI    0x0b   /* IOTG[7:4] @ [3:0] */
#define CX25601N_REG_VOTG_LO    0x0c   /* VOTG[1:0] @ [7:6] */
#define CX25601N_REG_VOTG_HI    0x0d   /* VOTG[6:2] @ [4:0] */
#define CX25601N_REG_IPRECHG_LO 0x10
#define CX25601N_REG_IPRECHG_HI 0x11
#define CX25601N_REG_ITERM_LO   0x12
#define CX25601N_REG_ITERM_HI   0x13
#define CX25601N_REG_CHG_CTRL0  0x14
#define CX25601N_REG_CHG_TMR    0x15
#define CX25601N_REG_CHG_CTRL1  0x16   /* EN_HIZ bit4, EN_CHG bit5, WDT[1:0] */
#define CX25601N_REG_CHG_CTRL3  0x18   /* EN_OTG bit6, BATFET_DLY bit2 */
#define CX25601N_REG_STATUS1    0x1e
#define CX25601N_REG_PART_INFO  0x38
#define CX25601N_REG_UNLOCK     0x70

#define CX25601N_OTG_ENTRY_DELAY_MS 30

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct i2c_master_s *g_i2c;
static bool g_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int cx_read(uint8_t reg, FAR uint8_t *val)
{
  struct i2c_msg_s msgs[2];
  int ret;

  msgs[0].frequency = 200000;
  msgs[0].addr      = BOARD_I2C_ADDR_CX25601N;
  msgs[0].flags     = 0;
  msgs[0].buffer    = &reg;
  msgs[0].length    = 1;

  msgs[1].frequency = 200000;
  msgs[1].addr      = BOARD_I2C_ADDR_CX25601N;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = val;
  msgs[1].length    = 1;

  ret = I2C_TRANSFER(g_i2c, msgs, 2);
  return ret < 0 ? ret : OK;
}

static int cx_write(uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[2] = { reg, val };
  int ret;

  msg.frequency = 200000;
  msg.addr      = BOARD_I2C_ADDR_CX25601N;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  ret = I2C_TRANSFER(g_i2c, &msg, 1);
  return ret < 0 ? ret : OK;
}

static int cx_update_bits(uint8_t reg, uint8_t mask, uint8_t shift,
                          uint8_t field)
{
  uint8_t cur = 0;
  uint8_t m;
  int ret;

  ret = cx_read(reg, &cur);
  if (ret < 0)
    {
      return ret;
    }

  m = (uint8_t)(mask << shift);
  cur = (uint8_t)((cur & (uint8_t)~m) | ((field << shift) & m));
  return cx_write(reg, cur);
}

static int cx_read_bits(uint8_t reg, uint8_t mask, uint8_t shift,
                        FAR uint8_t *field)
{
  uint8_t cur = 0;
  int ret;

  ret = cx_read(reg, &cur);
  if (ret < 0)
    {
      return ret;
    }

  *field = (uint8_t)((cur >> shift) & mask);
  return OK;
}

static void cx_unlock_private(bool enable)
{
  uint8_t v;
  int i;

  if (!enable)
    {
      cx_write(CX25601N_REG_UNLOCK, 0x00);
      return;
    }

  for (i = 0; i < 10; i++)
    {
      cx_write(CX25601N_REG_UNLOCK, 0x00);
      cx_write(CX25601N_REG_UNLOCK, 0x50);
      cx_write(CX25601N_REG_UNLOCK, 0x57);
      cx_write(CX25601N_REG_UNLOCK, 0x44);
      if (cx_read(CX25601N_REG_UNLOCK, &v) == OK && v == 0x03)
        {
          return;
        }
    }

  syslog(LOG_WARNING, "CX25601N: private register unlock failed\n");
}

static int cx_set_dis_dpdm(bool disable)
{
  /* REG0x15 bit6 EN_AUTO_INDET: disable→0, enable→1 */
  return cx_update_bits(CX25601N_REG_CHG_TMR, 0x01, 6, disable ? 0 : 1);
}

static int cx_set_iindpm_ma_nolock(uint32_t ma)
{
  uint32_t code;
  int ret;

  if (ma < CX25601N_IINDPM_MIN_MA)
    {
      ma = CX25601N_IINDPM_MIN_MA;
    }

  if (ma > CX25601N_IINDPM_MAX_MA)
    {
      ma = CX25601N_IINDPM_MAX_MA;
    }

  code = ma / CX25601N_IINDPM_STEP_MA;
  if (code < 5)
    {
      code = 5;
    }

  if (code > 150)
    {
      code = 150;
    }

  /* IINDPM[3:0] @ 0x06[7:4], IINDPM[7:4] @ 0x07[3:0] */
  ret = cx_update_bits(CX25601N_REG_IINDPM_LO, 0x0f, 4,
                       (uint8_t)(code & 0x0f));
  if (ret == OK)
    {
      ret = cx_update_bits(CX25601N_REG_IINDPM_HI, 0x0f, 0,
                           (uint8_t)((code >> 4) & 0x0f));
    }

  return ret;
}

static int cx_set_votg_mv_nolock(uint32_t mv)
{
  uint32_t code;
  int ret;

  if (mv < CX25601N_VOTG_MIN_MV)
    {
      mv = CX25601N_VOTG_MIN_MV;
    }

  if (mv > CX25601N_VOTG_MAX_MV)
    {
      mv = CX25601N_VOTG_MAX_MV;
    }

  code = mv / CX25601N_VOTG_STEP_MV;
  if (code < 0x30)
    {
      code = 0x30;
    }

  if (code > 0x42)
    {
      code = 0x42;
    }

  /* VOTG[1:0] @ 0x0C[7:6], VOTG[6:2] @ 0x0D[4:0] */
  ret = cx_update_bits(CX25601N_REG_VOTG_LO, 0x03, 6,
                       (uint8_t)(code & 0x03));
  if (ret == OK)
    {
      ret = cx_update_bits(CX25601N_REG_VOTG_HI, 0x1f, 0,
                           (uint8_t)((code >> 2) & 0x1f));
    }

  return ret;
}

static int cx_set_iotg_ma_nolock(uint32_t ma)
{
  uint32_t code;
  int ret;

  if (ma < CX25601N_IOTG_MIN_MA)
    {
      ma = CX25601N_IOTG_MIN_MA;
    }

  if (ma > CX25601N_IOTG_MAX_MA)
    {
      ma = CX25601N_IOTG_MAX_MA;
    }

  code = ma / CX25601N_IOTG_STEP_MA;
  if (code < 5)
    {
      code = 5;
    }

  if (code > 60)
    {
      code = 60;
    }

  /* IOTG[3:0] @ 0x0A[7:4], IOTG[7:4] @ 0x0B[3:0] */
  ret = cx_update_bits(CX25601N_REG_IOTG_LO, 0x0f, 4,
                       (uint8_t)(code & 0x0f));
  if (ret == OK)
    {
      ret = cx_update_bits(CX25601N_REG_IOTG_HI, 0x0f, 0,
                           (uint8_t)((code >> 4) & 0x0f));
    }

  return ret;
}

static int cx_hw_init_defaults(void)
{
  int ret;

  /* Disable D+/D- auto detection to avoid SDP 500mA current limiting */
  ret = cx_set_dis_dpdm(true);
  if (ret < 0)
    {
      return ret;
    }

  /* Disable HIZ & watchdog */
  ret = cx_update_bits(CX25601N_REG_CHG_CTRL1, 0x01, 4, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = cx_update_bits(CX25601N_REG_CHG_CTRL1, 0x03, 0, 0);
  if (ret < 0)
    {
      return ret;
    }

  /* IPRECHG = 12 * 20mA = 240mA */
  ret = cx_update_bits(CX25601N_REG_IPRECHG_LO, 0x0f, 4, 0x0c);
  if (ret < 0)
    {
      return ret;
    }

  ret = cx_update_bits(CX25601N_REG_IPRECHG_HI, 0x01, 0, 0x00);
  if (ret < 0)
    {
      return ret;
    }

  /* ITERM = 18 * 10mA = 180mA */
  ret = cx_update_bits(CX25601N_REG_ITERM_LO, 0x1f, 3, 0x12);
  if (ret < 0)
    {
      return ret;
    }

  ret = cx_update_bits(CX25601N_REG_ITERM_HI, 0x01, 0, 0x00);
  if (ret < 0)
    {
      return ret;
    }

  /* VREG default 4200mV, ICHG default 1000mA */
  ret = metalio_cx25601n_set_vreg_mv(4200);
  if (ret < 0)
    {
      return ret;
    }

  ret = metalio_cx25601n_set_ichg_ma(1000);
  if (ret < 0)
    {
      return ret;
    }

  /* Vendor private init sequence from reference */
  cx_unlock_private(true);
  cx_write(0x86, 0x06);
  cx_write(0x3a, 0x10);
  cx_write(0x46, 0x20);
  cx_unlock_private(false);

  cx_update_bits(CX25601N_REG_CHG_CTRL0, 0x01, 0, 1);
  cx_update_bits(CX25601N_REG_CHG_CTRL3, 0x01, 2, 0);
  cx_update_bits(0x1a, 0x01, 7, 1);
  cx_update_bits(0x23, 0x07, 2, 0x07);
  cx_update_bits(0x24, 0x01, 3, 1);

  syslog(LOG_INFO, "CX25601N: hw defaults applied\n");
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int metalio_cx25601n_initialize(FAR struct i2c_master_s *i2c)
{
  uint8_t part = 0;
  int ret;

  if (g_ready)
    {
      return OK;
    }

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  g_i2c = i2c;

  ret = cx_read(CX25601N_REG_PART_INFO, &part);
  if (ret < 0)
    {
      syslog(LOG_INFO,
             "CX25601N not found at 0x%02x (legacy board?), skip init: %d\n",
             BOARD_I2C_ADDR_CX25601N, ret);
      g_i2c = NULL;
      return ret;
    }

  syslog(LOG_INFO, "CX25601N PART_INFO=0x%02x @0x%02x\n",
         part, BOARD_I2C_ADDR_CX25601N);

  ret = cx_hw_init_defaults();
  if (ret < 0)
    {
      g_i2c = NULL;
      return ret;
    }

  g_ready = true;
  syslog(LOG_INFO, "CX25601N ready\n");
  return OK;
}

bool metalio_cx25601n_is_ready(void)
{
  return g_ready;
}

int metalio_cx25601n_read_reg(uint8_t reg, FAR uint8_t *val)
{
  if (!g_ready || val == NULL)
    {
      return -EINVAL;
    }

  return cx_read(reg, val);
}

int metalio_cx25601n_enable_charge(bool enable)
{
  int ret;

  if (!g_ready)
    {
      return -EINVAL;
    }

  if (enable)
    {
      ret = cx_update_bits(CX25601N_REG_CHG_CTRL1, 0x01, 4, 0); /* HIZ=0 */
      if (ret == OK)
        {
          ret = cx_update_bits(CX25601N_REG_CHG_CTRL1, 0x01, 5, 1); /* EN_CHG=1 */
        }
    }
  else
    {
      ret = cx_update_bits(CX25601N_REG_CHG_CTRL1, 0x01, 5, 0);
    }

  return ret;
}

int metalio_cx25601n_is_charge_enabled(FAR bool *enabled)
{
  uint8_t bit = 0;
  int ret;

  if (!g_ready || enabled == NULL)
    {
      return -EINVAL;
    }

  ret = cx_read_bits(CX25601N_REG_CHG_CTRL1, 0x01, 5, &bit);
  if (ret == OK)
    {
      *enabled = bit != 0;
    }

  return ret;
}

int metalio_cx25601n_enable_otg(bool enable)
{
  int ret;

  if (!g_ready)
    {
      return -EINVAL;
    }

  if (enable)
    {
      /* Align with reference: disable charge, configure 5V/1A, then EN_OTG */
      ret = cx_update_bits(CX25601N_REG_CHG_CTRL1, 0x01, 5, 0);
      if (ret == OK)
        {
          ret = cx_update_bits(CX25601N_REG_CHG_CTRL1, 0x01, 4, 0);
        }

      if (ret == OK)
        {
          ret = cx_set_votg_mv_nolock(CX25601N_OTG_DEFAULT_MV);
        }

      if (ret == OK)
        {
          ret = cx_set_iotg_ma_nolock(CX25601N_OTG_DEFAULT_MA);
        }

      if (ret == OK)
        {
          ret = cx_update_bits(CX25601N_REG_CHG_CTRL3, 0x01, 6, 1);
        }

      if (ret == OK)
        {
          up_mdelay(CX25601N_OTG_ENTRY_DELAY_MS);
        }

      return ret;
    }

  ret = cx_update_bits(CX25601N_REG_CHG_CTRL3, 0x01, 6, 0);
  if (ret == OK)
    {
      ret = cx_update_bits(CX25601N_REG_CHG_CTRL1, 0x01, 5, 1);
    }

  return ret;
}

int metalio_cx25601n_is_otg_enabled(FAR bool *enabled)
{
  uint8_t bit = 0;
  int ret;

  if (!g_ready || enabled == NULL)
    {
      return -EINVAL;
    }

  ret = cx_read_bits(CX25601N_REG_CHG_CTRL3, 0x01, 6, &bit);
  if (ret == OK)
    {
      *enabled = bit != 0;
    }

  return ret;
}

int metalio_cx25601n_set_ichg_ma(uint32_t ma)
{
  uint32_t code;
  uint32_t applied_ma;
  int ret;

  if (g_i2c == NULL)
    {
      return -EINVAL;
    }

  if (ma < CX25601N_ICHG_MIN_MA)
    {
      ma = CX25601N_ICHG_MIN_MA;
    }

  if (ma > CX25601N_ICHG_MAX_MA)
    {
      ma = CX25601N_ICHG_MAX_MA;
    }

  code = ma / CX25601N_ICHG_STEP_MA;
  if (code < 1)
    {
      code = 1;
    }

  if (code > 38)
    {
      code = 38;
    }

  applied_ma = code * CX25601N_ICHG_STEP_MA;

  /* ICHG[1:0] @ 0x02[7:6], ICHG[5:2] @ 0x03[3:0] */
  ret = cx_update_bits(CX25601N_REG_ICHG_LO, 0x03, 6,
                       (uint8_t)(code & 0x03));
  if (ret == OK)
    {
      ret = cx_update_bits(CX25601N_REG_ICHG_HI, 0x0f, 0,
                           (uint8_t)((code >> 2) & 0x0f));
    }

  /* Apply input current limit in lock-step to avoid SDP limiting */
  if (ret == OK)
    {
      ret = cx_set_iindpm_ma_nolock(applied_ma);
    }

  return ret;
}

int metalio_cx25601n_get_ichg_ma(FAR uint32_t *ma)
{
  uint8_t lo = 0;
  uint8_t hi = 0;
  uint32_t code;
  int ret;

  if (!g_ready || ma == NULL)
    {
      return -EINVAL;
    }

  ret = cx_read_bits(CX25601N_REG_ICHG_LO, 0x03, 6, &lo);
  if (ret == OK)
    {
      ret = cx_read_bits(CX25601N_REG_ICHG_HI, 0x0f, 0, &hi);
    }

  if (ret != OK)
    {
      return ret;
    }

  code = (uint32_t)lo | ((uint32_t)hi << 2);
  *ma = code * CX25601N_ICHG_STEP_MA;
  return OK;
}

int metalio_cx25601n_set_iindpm_ma(uint32_t ma)
{
  if (g_i2c == NULL)
    {
      return -EINVAL;
    }

  return cx_set_iindpm_ma_nolock(ma);
}

int metalio_cx25601n_get_iindpm_ma(FAR uint32_t *ma)
{
  uint8_t lo = 0;
  uint8_t hi = 0;
  uint32_t code;
  int ret;

  if (!g_ready || ma == NULL)
    {
      return -EINVAL;
    }

  ret = cx_read_bits(CX25601N_REG_IINDPM_LO, 0x0f, 4, &lo);
  if (ret == OK)
    {
      ret = cx_read_bits(CX25601N_REG_IINDPM_HI, 0x0f, 0, &hi);
    }

  if (ret != OK)
    {
      return ret;
    }

  code = (uint32_t)lo | ((uint32_t)hi << 4);
  *ma = code * CX25601N_IINDPM_STEP_MA;
  return OK;
}

int metalio_cx25601n_set_vreg_mv(uint32_t mv)
{
  uint32_t code;
  int ret;

  if (g_i2c == NULL)
    {
      return -EINVAL;
    }

  if (mv < CX25601N_VREG_MIN_MV)
    {
      mv = CX25601N_VREG_MIN_MV;
    }

  if (mv > CX25601N_VREG_MAX_MV)
    {
      mv = CX25601N_VREG_MAX_MV;
    }

  /* VREG = code * 10 mV, code range 384..480 */
  code = mv / CX25601N_VREG_STEP_MV;

  /* VREG[4:0] @ 0x04[7:3], VREG[8:5] @ 0x05[3:0] */
  ret = cx_update_bits(CX25601N_REG_VREG_LO, 0x1f, 3,
                       (uint8_t)(code & 0x1f));
  if (ret == OK)
    {
      ret = cx_update_bits(CX25601N_REG_VREG_HI, 0x0f, 0,
                           (uint8_t)((code >> 5) & 0x0f));
    }

  return ret;
}

int metalio_cx25601n_get_vreg_mv(FAR uint32_t *mv)
{
  uint8_t lo = 0;
  uint8_t hi = 0;
  uint32_t code;
  int ret;

  if (!g_ready || mv == NULL)
    {
      return -EINVAL;
    }

  ret = cx_read_bits(CX25601N_REG_VREG_LO, 0x1f, 3, &lo);
  if (ret == OK)
    {
      ret = cx_read_bits(CX25601N_REG_VREG_HI, 0x0f, 0, &hi);
    }

  if (ret != OK)
    {
      return ret;
    }

  code = (uint32_t)lo | ((uint32_t)hi << 5);
  *mv = code * CX25601N_VREG_STEP_MV;
  return OK;
}

int metalio_cx25601n_get_chrg_stat(FAR uint8_t *stat)
{
  if (!g_ready || stat == NULL)
    {
      return -EINVAL;
    }

  return cx_read_bits(CX25601N_REG_STATUS1, 0x03, 3, stat);
}

int metalio_cx25601n_get_vbus_stat(FAR uint8_t *stat)
{
  if (!g_ready || stat == NULL)
    {
      return -EINVAL;
    }

  return cx_read_bits(CX25601N_REG_STATUS1, 0x07, 0, stat);
}

FAR const char *metalio_cx25601n_chrg_stat_str(uint8_t stat)
{
  switch (stat)
    {
    case CX25601N_CHG_STAT_NOT:
      return "未充电/已满";
    case CX25601N_CHG_STAT_CC:
      return "涓流/预充/CC";
    case CX25601N_CHG_STAT_CV:
      return "恒压降流";
    case CX25601N_CHG_STAT_TOPOFF:
      return "Top-off";
    default:
      return "未知";
    }
}

FAR const char *metalio_cx25601n_vbus_stat_str(uint8_t stat)
{
  switch (stat)
    {
    case 0:
      return "无输入";
    case 1:
      return "USB SDP";
    case 2:
      return "USB CDP";
    case 3:
      return "USB DCP";
    case 4:
      return "未知适配器";
    case 5:
      return "非标适配器";
    case 7:
      return "OTG";
    default:
      return "适配器";
    }
}
