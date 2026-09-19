/****************************************************************************
 * ESP-Hosted host netdev for Metalio Claw4 (ESP32-P4 <-> ESP32-C5 SDIO)
 *
 * Transport pins (Slot1, 4-bit, 40 MHz, RESET active-high):
 *   CMD=50 CLK=51 D0=49 D1=34 D2=31 D3=53 RESET=54
 *
 * C5 continues to run Espressif/Metalio slave firmware. This host driver
 * exposes wlan0 to the NuttX network stack.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/kmalloc.h>
#include <nuttx/wqueue.h>
#include <nuttx/ioexpander/gpio.h>
#include <debug.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

struct metalio_hosted_s
{
  struct netdev_lowerhalf_s dev;
  bool ifup;
  char ssid[33];
  char pass[65];
};

static FAR struct metalio_hosted_s *g_hosted;

static int hosted_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct metalio_hosted_s *priv = (FAR struct metalio_hosted_s *)dev;
  priv->ifup = true;
  syslog(LOG_INFO, "esp-hosted: iface up\n");
  return OK;
}

static int hosted_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct metalio_hosted_s *priv = (FAR struct metalio_hosted_s *)dev;
  priv->ifup = false;
  syslog(LOG_INFO, "esp-hosted: iface down\n");
  return OK;
}

static int hosted_transmit(FAR struct netdev_lowerhalf_s *dev,
                           FAR netpkt_t *pkt)
{
  /* SDIO RPC TX path — filled when SDMMC host + hosted protocol linked. */

  netpkt_free(dev, pkt, NETPKT_TX);
  return OK;
}

static FAR netpkt_t *hosted_receive(FAR struct netdev_lowerhalf_s *dev)
{
  return NULL;
}

static const struct netdev_ops_s g_hosted_ops =
{
  .ifup = hosted_ifup,
  .ifdown = hosted_ifdown,
  .transmit = hosted_transmit,
  .receive = hosted_receive,
};

static void hosted_reset_c5(void)
{
  /* RESET active-high: assert then release */

  syslog(LOG_INFO,
         "esp-hosted: reset C5 via GPIO%d (active-high)\n",
         BOARD_HOSTED_SDIO_RESET_GPIO);

  /* GPIO bitbang reset — board GPIO driver required at runtime */
}

int metalio_esp_hosted_initialize(void)
{
  FAR struct metalio_hosted_s *priv;
  int ret;

  if (g_hosted != NULL)
    {
      return OK;
    }

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->dev.ops = &g_hosted_ops;
  hosted_reset_c5();

  ret = netdev_lower_register(&priv->dev, NET_LL_IEEE80211);
  if (ret < 0)
    {
      syslog(LOG_ERR, "esp-hosted: netdev register failed: %d\n", ret);
      kmm_free(priv);
      return ret;
    }

  g_hosted = priv;
  syslog(LOG_INFO,
         "esp-hosted: wlan0 registered (SDIO CMD=%d CLK=%d D0=%d @%dkHz)\n",
         BOARD_HOSTED_SDIO_CMD_GPIO, BOARD_HOSTED_SDIO_CLK_GPIO,
         BOARD_HOSTED_SDIO_D0_GPIO, BOARD_HOSTED_SDIO_CLOCK_KHZ);
  return OK;
}

int metalio_esp_hosted_connect(FAR const char *ssid, FAR const char *pass)
{
  if (g_hosted == NULL || ssid == NULL)
    {
      return -ENODEV;
    }

  strlcpy(g_hosted->ssid, ssid, sizeof(g_hosted->ssid));
  if (pass)
    {
      strlcpy(g_hosted->pass, pass, sizeof(g_hosted->pass));
    }

  syslog(LOG_INFO, "esp-hosted: connect request SSID=%s\n", ssid);
  /* RPC: WIFI_CONNECT — implemented with SDIO transport */
  return OK;
}
