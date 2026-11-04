#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <stdio.h>
#include <stdatomic.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>
#include "espressif/esp_gpio.h"

static mutex_t g_bt_mode_lock = NXMUTEX_INITIALIZER;
static atomic_bool g_bt_voice_ready = false;
static atomic_bool g_bt_powered = true;
static atomic_bool g_bt_scan_mode = false;

void metalio_bt_set_scan_mode(bool enabled)
{
  atomic_store(&g_bt_scan_mode, enabled);
  g_bt_voice_ready = false;
}

/* The app's unsolicited-response reader and synchronous mode transactions
 * share one UART. A reader must never consume a mode transaction's ACK. */
int metalio_bt_transport_trylock(void)
{
  return nxmutex_trylock(&g_bt_mode_lock);
}

void metalio_bt_transport_unlock(void)
{
  nxmutex_unlock(&g_bt_mode_lock);
}

bool metalio_bt_is_powered(void)
{
  return atomic_load(&g_bt_powered);
}

bool metalio_bt_voice_ready(void)
{
  return g_bt_voice_ready;
}

void metalio_bt_leave_voice(void)
{
  g_bt_voice_ready = false;
}

static void bt_expect(int fd, const char *tag, int timeout_ms);

static bool bt_apply_mode1_locked(int fd, int attempt_tag)
{
  char acc[160];
  size_t acc_len = 0;
  int waited;
  ssize_t ret;
  bool mode1_ok = false;

  if (attempt_tag > 0)
    {
      syslog(LOG_INFO, "BT audio: mode 1 retry %d\n", attempt_tag);
      usleep(600 * 1000);
    }

  ret = write(fd, "AT+RX=2\r\n", 9);
  syslog(LOG_INFO, "BT TX AT+RX=2 -> %d\n", (int)ret);

  usleep(700 * 1000);
  bt_expect(fd, "after RX=2", 400);

  ret = write(fd, "AT+MODE=1\r\n", 11);
  syslog(LOG_INFO, "BT TX AT+MODE=1 -> %d\n", (int)ret);

  acc[0] = '\0';
  for (waited = 0; waited < 1200 && !mode1_ok; waited += 200)
    {
      struct pollfd pfd;
      ssize_t r;

      pfd.fd = fd;
      pfd.events = POLLIN;
      pfd.revents = 0;

      if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN))
        {
          r = read(fd, acc + acc_len, sizeof(acc) - 1 - acc_len);
          if (r > 0)
            {
              acc_len += (size_t)r;
              acc[acc_len] = '\0';
              if (strstr(acc, "SET MODE 1") != NULL)
                {
                  mode1_ok = true;
                }
            }
        }
    }

  if (acc_len > 0)
    {
      syslog(LOG_INFO, "BT RX mode1 window: %s\n", acc);
    }

  return mode1_ok;
}

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
  bool mode1_ok = false;
  int attempt;

  /* Preserve the codec rail across CPU resets. The reference board only
   * power-cycles BT_POWER for firmware/download maintenance. */
  metalio_tca9555_write_pin(BOARD_IOEXP_BT_POWER, true);
  syslog(LOG_INFO, "BT audio UART TX=%d RX=%d 115200\n",
         BOARD_BT_AUDIO_TX_GPIO, BOARD_BT_AUDIO_RX_GPIO);

  {
    bool powered = false;
    int check = metalio_tca9555_read_pin(BOARD_IOEXP_BT_POWER, &powered);
    char message[64];
    int n = snprintf(message, sizeof(message), "BT_POWER read=%d level=%d\n", check, powered);
    write(1, message, n);
  }

  /* The BT codec needs time to boot after its power rail is enabled before it
   * can accept AT commands. The reference firmware powers it in
   * InitializeIOExpander() well before InitializeBTAudio() sends commands, so
   * by the time the AT sequence runs the codec has had the whole display init
   * as warm-up. Keep a short settle — 5s blocked the USB console and looked
   * like a hang right after GT911. */
  usleep(1200 * 1000);

  /* The Bluetooth audio codec chip does not enter its I2S audio mode on
   * power-up by itself; the reference BluetoothScreen::ApplyDefaultMode()
   * sends the following AT sequence over UART2 to switch it into mode 1
   * (voice-assistant receive / I2S audio). Without mode 1 the codec never
   * drives the I2S BCLK/WS lines (P4 is the slave) and BOTH mic capture and
   * speaker playback starve — so retry until the codec ACKs "SET MODE 1"
   * instead of firing the sequence blindly. */
  fd = open("/dev/ttyS1", O_RDWR);
  if (fd < 0)
    {
      syslog(LOG_WARNING, "BT audio: open /dev/ttyS1 failed: %d\n", errno);
      return -errno;
    }

  for (attempt = 0; attempt < 3 && !mode1_ok; attempt++)
    {
      mode1_ok = bt_apply_mode1_locked(fd, attempt);
    }

  g_bt_voice_ready = mode1_ok;
  write(1, mode1_ok ? "BT_MODE1\n" : "BT_MODE1_NACK\n",
        mode1_ok ? 9 : 14);

  /* Give the codec time to actually enter I2S master mode and start driving
   * BCLK/WS before the I2S lower half is used. */
  usleep(1200 * 1000);

  close(fd);
  syslog(LOG_INFO, "BT audio: default mode 1 %s\n",
         mode1_ok ? "applied" : "NOT confirmed");
  return OK;
}

void metalio_bt_probe_clocks(void)
{
  /* The BT codec is the I2S clock master (P4 is slave). If it never entered
   * I2S audio mode, BCLK/WS stay static and mic capture starves with a
   * forever-blocking read. Count edges for a few ms to tell "codec not
   * driving clocks" apart from an I2S driver fault. Call only after the I2S
   * pins are routed (board_i2s_init) so the pad input path is live. */
  bool last_bclk = esp_gpioread(BOARD_I2S_SPK_BCLK_GPIO);
  bool last_ws   = esp_gpioread(BOARD_I2S_MIC_WS_GPIO);
  int bclk_edges = 0;
  int ws_edges   = 0;
  int i;

  for (i = 0; i < 120000; i++)
    {
      bool b = esp_gpioread(BOARD_I2S_SPK_BCLK_GPIO);
      bool w = esp_gpioread(BOARD_I2S_MIC_WS_GPIO);
      if (b != last_bclk)
        {
          bclk_edges++;
          last_bclk = b;
        }
      if (w != last_ws)
        {
          ws_edges++;
          last_ws = w;
        }
    }

  syslog(LOG_INFO, "BT_CLK bclk_edges=%d ws_edges=%d\n",
         bclk_edges, ws_edges);
  if (bclk_edges > 100 && ws_edges > 4)
    {
      write(1, "BT_CLK_OK\n", 10);
    }
  else
    {
      write(1, "BT_CLK_NONE\n", 12);
    }
}

int metalio_bt_accept_headset_stream(void)
{
  nxmutex_lock(&g_bt_mode_lock);
  int bclk=0,ws=0;bool b=esp_gpioread(BOARD_I2S_SPK_BCLK_GPIO),w=esp_gpioread(BOARD_I2S_MIC_WS_GPIO);
  for(int i=0;i<120000;i++){bool nb=esp_gpioread(BOARD_I2S_SPK_BCLK_GPIO),nw=esp_gpioread(BOARD_I2S_MIC_WS_GPIO);if(nb!=b){++bclk;b=nb;}if(nw!=w){++ws;w=nw;}}
  bool ready=atomic_load(&g_bt_powered)&&bclk>100&&ws>4;atomic_store(&g_bt_voice_ready,ready);
  nxmutex_unlock(&g_bt_mode_lock);return ready?OK:-EIO;
}

int metalio_bt_power(bool on)
{
  bool level = !on;
  nxmutex_lock(&g_bt_mode_lock);
  const bool changed = atomic_load(&g_bt_powered) != on;
  int ret = metalio_tca9555_write_pin(BOARD_IOEXP_BT_POWER, on);
  if (ret == OK) ret = metalio_tca9555_read_pin(BOARD_IOEXP_BT_POWER, &level);
  if (ret == OK && level != on) ret = -EIO;
  if (ret == OK) {
    atomic_store(&g_bt_powered, on);
    if (changed) g_bt_voice_ready = false;
  }
  nxmutex_unlock(&g_bt_mode_lock);
  return ret;
}

int metalio_bt_ensure_mode1(void)
{
  int fd;
  bool mode1_ok = false;
  int attempt;

  nxmutex_lock(&g_bt_mode_lock);
  if (atomic_load(&g_bt_scan_mode)) {
    nxmutex_unlock(&g_bt_mode_lock);
    return -EBUSY;
  }
  if (!atomic_load(&g_bt_powered)) {
    nxmutex_unlock(&g_bt_mode_lock);
    return -ENETDOWN;
  }
  if (atomic_load(&g_bt_voice_ready)) {
    nxmutex_unlock(&g_bt_mode_lock);
    return OK;
  }
  g_bt_voice_ready = false;

  metalio_tca9555_write_pin(BOARD_IOEXP_BT_POWER, true);
  metalio_tca9555_write_pin(BOARD_IOEXP_PA_ENABLE, true);

  fd = open("/dev/ttyS1", O_RDWR);
  if (fd < 0)
    {
      write(1, "BT_REMODE_FAIL\n", 15);
      nxmutex_unlock(&g_bt_mode_lock);
      return -errno;
    }

  for (attempt = 0; attempt < 3 && !mode1_ok; attempt++)
    {
      mode1_ok = bt_apply_mode1_locked(fd, attempt);
    }

  usleep(800 * 1000);
  close(fd);

  g_bt_voice_ready = mode1_ok;
  write(1, mode1_ok ? "BT_REMODE_OK\n" : "BT_REMODE_NACK\n",
        mode1_ok ? 13 : 15);
  if (mode1_ok)
    {
      metalio_bt_probe_clocks();
    }

  nxmutex_unlock(&g_bt_mode_lock);
  return mode1_ok ? OK : -EIO;
}

int metalio_bt_enter_music(void)
{
  int fd;
  ssize_t ret;

  nxmutex_lock(&g_bt_mode_lock);
  if (!atomic_load(&g_bt_powered)) {
    nxmutex_unlock(&g_bt_mode_lock);
    return -ENETDOWN;
  }
  g_bt_voice_ready = false;

  fd = open("/dev/ttyS1", O_RDWR);
  if (fd < 0)
    {
      nxmutex_unlock(&g_bt_mode_lock);
      return -errno;
    }

  ret = write(fd, "AT+RX=1\r\n", 9);
  syslog(LOG_INFO, "BT TX AT+RX=1 -> %d\n", (int)ret);
  usleep(700 * 1000);
  ret = write(fd, "AT+MODE=3\r\n", 11);
  syslog(LOG_INFO, "BT TX AT+MODE=3 -> %d\n", (int)ret);
  usleep(400 * 1000);
  close(fd);

  nxmutex_unlock(&g_bt_mode_lock);
  return OK;
}

int metalio_i2s_audio_initialize(void)
{
  metalio_tca9555_write_pin(BOARD_IOEXP_PA_ENABLE, true);
  syslog(LOG_INFO, "I2S audio MIC WS=%d DIN=%d SPK DOUT=%d BCLK=%d\n",
         BOARD_I2S_MIC_WS_GPIO, BOARD_I2S_MIC_DIN_GPIO,
         BOARD_I2S_SPK_DOUT_GPIO, BOARD_I2S_SPK_BCLK_GPIO);
  return OK;
}
