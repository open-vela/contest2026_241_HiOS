#include <nuttx/config.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>

/* Wait (bounded) for the codec to send back an AT response and log it, so we
 * can tell whether the module actually received and acknowledged the command.
 * The codec replies with "SET MODE 1" after AT+MODE=1 succeeds. */
static void bt_expect(int fd, const char *tag, int timeout_ms)
{
  struct pollfd pfd;
  char buf[128];
  ssize_t n;

  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  n = poll(&pfd, 1, timeout_ms);
  if (n > 0 && (pfd.revents & POLLIN))
    {
      n = read(fd, buf, sizeof(buf) - 1);
      if (n > 0)
        {
          buf[n] = '\0';
          syslog(LOG_INFO, "BT RX %s: %s\n", tag, buf);
        }
    }
  else if (n == 0)
    {
      syslog(LOG_INFO, "BT RX %s: (no response in %dms)\n", tag, timeout_ms);
    }
  else
    {
      syslog(LOG_INFO, "BT RX %s: poll failed: %d\n", tag, errno);
    }
}

int metalio_bt_audio_initialize(void)
{
  int fd;
  ssize_t ret;

  metalio_tca9555_write_pin(BOARD_IOEXP_BT_POWER, true);
  syslog(LOG_INFO, "BT audio UART TX=%d RX=%d 115200\n",
         BOARD_BT_AUDIO_TX_GPIO, BOARD_BT_AUDIO_RX_GPIO);

  /* The BT codec needs time to boot after its power rail is enabled before it
   * can accept AT commands. The reference firmware powers it in
   * InitializeIOExpander() well before InitializeBTAudio() sends commands, so
   * by the time the AT sequence runs the codec has had the whole display init
   * as warm-up. Keep a short settle — 5s blocked the USB console and looked
   * like a hang right after GT911. */
  usleep(800 * 1000);

  /* The Bluetooth audio codec chip does not enter its I2S audio mode on
   * power-up by itself; the reference BluetoothScreen::ApplyDefaultMode()
   * sends the following AT sequence over UART2 to switch it into mode 1
   * (voice-assistant receive / I2S audio). */
  fd = open("/dev/ttyS1", O_RDWR);
  if (fd < 0)
    {
      syslog(LOG_WARNING, "BT audio: open /dev/ttyS1 failed: %d\n", errno);
      return -errno;
    }

  ret = write(fd, "AT+RX=2\r\n", 9);
  syslog(LOG_INFO, "BT TX AT+RX=2 -> %d\n", (int)ret);

  usleep(700 * 1000);
  bt_expect(fd, "after RX=2", 1000);

  ret = write(fd, "AT+MODE=1\r\n", 11);
  syslog(LOG_INFO, "BT TX AT+MODE=1 -> %d\n", (int)ret);

  usleep(700 * 1000);
  bt_expect(fd, "after MODE=1", 1000);

  /* Give the codec time to actually enter I2S master mode and start driving
   * BCLK/WS before the I2S lower half is used. */
  usleep(1500 * 1000);

  close(fd);
  syslog(LOG_INFO, "BT audio: default mode 1 applied\n");
  return OK;
}

int metalio_bt_power(bool on)
{
  return metalio_tca9555_write_pin(BOARD_IOEXP_BT_POWER, on);
}

int metalio_i2s_audio_initialize(void)
{
  metalio_tca9555_write_pin(BOARD_IOEXP_PA_ENABLE, true);
  syslog(LOG_INFO, "I2S audio MIC WS=%d DIN=%d SPK DOUT=%d BCLK=%d\n",
         BOARD_I2S_MIC_WS_GPIO, BOARD_I2S_MIC_DIN_GPIO,
         BOARD_I2S_SPK_DOUT_GPIO, BOARD_I2S_SPK_BCLK_GPIO);
  return OK;
}
