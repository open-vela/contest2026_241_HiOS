#include <nuttx/config.h>
#include <syslog.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

int metalio_nt26_initialize(void)
{
  metalio_tca9555_write_pin(BOARD_IOEXP_RST_4G, true);
  syslog(LOG_INFO, "NT26 4G UART TX=%d RX=%d MRDY=%d SRDY=%d\n",
         BOARD_NT26_TX_GPIO, BOARD_NT26_RX_GPIO,
         BOARD_NT26_MRDY_GPIO, BOARD_NT26_SRDY_GPIO);
  return OK;
}
