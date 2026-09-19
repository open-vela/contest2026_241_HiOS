/****************************************************************************
 * Dual-network helper: ESP-Hosted Wi-Fi primary + NT26 4G secondary
 ****************************************************************************/

#include <nuttx/config.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

enum metalio_net_path_e
{
  METALIO_NET_WIFI = 0,
  METALIO_NET_CELLULAR = 1
};

static enum metalio_net_path_e g_path = METALIO_NET_WIFI;

int metalio_dual_network_initialize(void)
{
  int ret;

  ret = metalio_esp_hosted_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "dual-net: Wi-Fi host init failed: %d\n", ret);
    }

#ifdef CONFIG_METALIO_NT26_4G
  ret = metalio_nt26_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "dual-net: NT26 init failed: %d\n", ret);
    }
#endif

  g_path = METALIO_NET_WIFI;
  syslog(LOG_INFO, "dual-net: default path = Wi-Fi (ESP-Hosted)\n");
  return OK;
}

int metalio_dual_network_select(int cellular)
{
  if (cellular)
    {
      g_path = METALIO_NET_CELLULAR;
      metalio_tca9555_write_pin(BOARD_IOEXP_PA_SWITCH, false); /* 4G audio path */
      syslog(LOG_INFO, "dual-net: switched to cellular\n");
    }
  else
    {
      g_path = METALIO_NET_WIFI;
      metalio_tca9555_write_pin(BOARD_IOEXP_PA_SWITCH, true);
      syslog(LOG_INFO, "dual-net: switched to Wi-Fi\n");
    }

  return OK;
}

int metalio_dual_network_path(void)
{
  return (int)g_path;
}
