#include <nuttx/config.h>
#include <syslog.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

int metalio_camera_initialize(void)
{
  /* CAM_PWDN active low */

  metalio_tca9555_write_pin(BOARD_IOEXP_CAM_PWDN, false);
  syslog(LOG_INFO, "OV2710 MIPI-CSI power on (CSI driver pending upstream)\n");
  return OK;
}
