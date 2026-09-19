#include <nuttx/config.h>
#include <nuttx/serial/serial.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

int metalio_gps_initialize(void)
{
  metalio_tca9555_write_pin(BOARD_IOEXP_GPS_POWER, true);
  syslog(LOG_INFO, "GPS power on, UART TX=%d RX=%d 9600\n",
         BOARD_GPS_TX_GPIO, BOARD_GPS_RX_GPIO);
  return OK;
}
