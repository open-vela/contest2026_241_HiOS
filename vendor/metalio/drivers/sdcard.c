#include <nuttx/config.h>
#include <syslog.h>
#include <errno.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

int metalio_sdcard_initialize(void)
{
  /* Enable SD power via TCA9555 (active low), then mount SDMMC. */

  metalio_tca9555_write_pin(BOARD_IOEXP_SD_POWER, false);
  syslog(LOG_INFO, "SDMMC pins CLK=%d CMD=%d D0=%d (power on)\n",
         BOARD_SDMMC_CLK_GPIO, BOARD_SDMMC_CMD_GPIO, BOARD_SDMMC_D0_GPIO);
  return OK;
}
