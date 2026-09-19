/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/esp32p4_bringup.c
 *
 * Metalio Claw4 board bring-up: I2C, IO expander, power, optional display,
 * touch, ESP-Hosted Wi-Fi, SD, GPS, 4G.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <syslog.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include <nuttx/fs/fs.h>
#include <nuttx/i2c/i2c_master.h>

#include "metalio-claw-4.h"
#include <arch/board/board.h>

#ifdef CONFIG_ESPRESSIF_I2C
#  include "espressif/esp_i2c.h"
#endif

#ifdef CONFIG_WATCHDOG
#  include "espressif/esp_wdt.h"
#endif

#ifdef CONFIG_TIMER
#  include "espressif/esp_gptimer.h"
#endif

#ifdef CONFIG_RTC_DRIVER
#  include "espressif/esp_rtc.h"
#endif

#ifdef CONFIG_DEV_GPIO
#  include "espressif/esp_gpio.h"
#endif

/* GPIO API is always available on ESP32-P4, even without CONFIG_DEV_GPIO
 * (the lower-level esp_configgpio/esp_gpiowrite are part of the arch). */
#include "espressif/esp_gpio.h"

/* Vendor Metalio drivers (linked when present in apps/vendor build) */

#ifdef CONFIG_METALIO_ESP_HOSTED
int metalio_esp_hosted_initialize(void);
#endif

#ifdef CONFIG_ESPRESSIF_I2S
/* From boards/risc-v/esp32p4/common — registers /dev/audio/pcm0 + pcm_in0 */
extern int board_i2s_init(void);
#endif

#ifdef CONFIG_METALIO_TOUCH_GT911
int metalio_gt911_initialize(FAR struct i2c_master_s *i2c);
#endif

#if defined(CONFIG_METALIO_DISPLAY_NV3051F) || defined(CONFIG_METALIO_DISPLAY_FL7707N)
int metalio_display_initialize(void);
#endif

#ifdef CONFIG_ESPRESSIF_LEDC
int board_ledc_setup(void);
#endif

int metalio_tca9555_initialize(FAR struct i2c_master_s *i2c);
int metalio_bq27220_initialize(FAR struct i2c_master_s *i2c);
int metalio_nu1680_initialize(FAR struct i2c_master_s *i2c);
int metalio_cx25601n_initialize(FAR struct i2c_master_s *i2c);
int metalio_sc7a20h_initialize(FAR struct i2c_master_s *i2c);
int metalio_qmc6309_initialize(FAR struct i2c_master_s *i2c);
int metalio_gps_initialize(void);
int metalio_sdcard_initialize(void);
int metalio_camera_initialize(FAR struct i2c_master_s *i2c);
#ifdef CONFIG_METALIO_CAMERA_OV2710
int metalio_camera_capture_test(void);
#endif

int metalio_bq27220_read_soc(FAR int *percent);
int metalio_tca9555_read_pin(int pin, FAR bool *level);
int metalio_pwr_shutdown_pulse(void);

/****************************************************************************
 * Name: metalio_check_battery_at_boot
 *
 * Mirror METALIO_CLAW_4::CheckBatteryLevelAtBoot(): if the fuel gauge
 * reads 0% and no charger (USB / wireless) is present, pulse the power key
 * to force a clean shutdown, preventing deep-discharge of the cell.
 ****************************************************************************/

static void metalio_check_battery_at_boot(void)
{
  int level = 0;
  int attempt;

  for (attempt = 0; attempt < 5; attempt++)
    {
      if (metalio_bq27220_read_soc(&level) == 0)
        {
          bool usb = false;
          bool wchg = false;

          (void)metalio_tca9555_read_pin(BOARD_IOEXP_USB_INSERT, &usb);
          (void)metalio_tca9555_read_pin(BOARD_IOEXP_WIRELESS_CHG, &wchg);

          syslog(LOG_INFO, "Boot battery: %d%% charger=%s\n",
                 level, (usb || wchg) ? "yes" : "no");

          if (level == 0 && !usb && !wchg)
            {
              syslog(LOG_WARNING, "Battery 0%%, forcing power off\n");
              (void)metalio_pwr_shutdown_pulse();
              for (; ; )
                {
                  up_mdelay(1000);
                }
            }

          return;
        }

      up_mdelay(100);
    }

  syslog(LOG_WARNING, "Boot battery check: gauge unavailable, skip\n");
}

/****************************************************************************
 * Name: esp_bringup
 ****************************************************************************/

int esp_bringup(void)
{
  int ret = OK;
  FAR struct i2c_master_s *i2c = NULL;

#ifdef CONFIG_FS_PROCFS
  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs: %d\n", ret);
    }
#endif

#ifdef CONFIG_WATCHDOG
  ret = esp_wdt_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "Failed to initialize WDT: %d\n", ret);
    }
#endif

#ifdef CONFIG_TIMER
  ret = esp_gptimer_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "Failed to initialize GPTimer: %d\n", ret);
    }
#endif

#ifdef CONFIG_RTC_DRIVER
  ret = esp_rtc_driverinit();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "Failed to initialize RTC driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  ret = esp_gpio_init();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "Failed to initialize GPIO driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_I2C0
  i2c = esp_i2cbus_initialize(0);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "Failed to init I2C0 (SDA=%d SCL=%d)\n",
             BOARD_I2C0_SDA_GPIO, BOARD_I2C0_SCL_GPIO);
    }
  else
    {
      /* Power / IO expander chain */
      ret = metalio_tca9555_initialize(i2c);
      ret |= metalio_bq27220_initialize(i2c);
      ret |= metalio_nu1680_initialize(i2c);
      ret |= metalio_cx25601n_initialize(i2c);
      ret |= metalio_sc7a20h_initialize(i2c);
      ret |= metalio_qmc6309_initialize(i2c);
      syslog(LOG_INFO, "I2C0+periph ready\n");

      /* Boot battery protection — force shutdown if the cell is empty. */
      metalio_check_battery_at_boot();

      /* OV2710 camera SCCB self-test — power the sensor on, probe it on the
       * shared I2C bus and read its chip ID, then power it back off (there
       * is no MIPI-CSI data path yet). */
      ret = metalio_camera_initialize(i2c);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "OV2710 camera SCCB init failed: %d\n", ret);
        }

      /* GT911 touch is initialized AFTER the display, matching the
       * original MetalioClaw4 boot order:
       *   "LCD 上电稳定后再初始化 GT911" — the touch controller may
       *   share a power/reset domain with the LCD panel. */
    }
#endif /* CONFIG_ESPRESSIF_I2C0 */

#if defined(CONFIG_METALIO_DISPLAY_NV3051F) || defined(CONFIG_METALIO_DISPLAY_FL7707N)
  ret = metalio_display_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "Display init failed: %d\n", ret);
    }
#endif

  /* GT911 touch — initialized after display power is stable.
   * Need to re-acquire the I2C bus handle since it's scoped above. */

#ifdef CONFIG_METALIO_TOUCH_GT911
#ifdef CONFIG_ESPRESSIF_I2C0
  if (i2c != NULL)
    {
      /* LCD reset (GPIO3) is now handled inside metalio_display_initialize(),
       * which runs before this point.  Only reset GT911 here. */

      /* GT911 reset via TCA9555 P1.6 (BOARD_IOEXP_TOUCH_RST, active-low).
       * The IO expander already releases this line at power-up; pulse it
       * low then high here so GT911 performs a clean reset after the LCD
       * is up and starts ACKing on 0x5d/0x14. */
      metalio_tca9555_write_pin(BOARD_IOEXP_TOUCH_RST, false);
      up_mdelay(20);
      metalio_tca9555_write_pin(BOARD_IOEXP_TOUCH_RST, true);
      up_mdelay(200);                        /* let GT911 finish internal init */

      ret = metalio_gt911_initialize(i2c);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "GT911 not found (no ACK at 0x5d/0x14)\n");
        }

      /* I2C scan disabled — too verbose for USB serial */
#if 0
      if (ret < 0)
        {
          … I2C scan code …
        }
#endif
    }
#endif /* CONFIG_ESPRESSIF_I2C0 */
#endif /* CONFIG_METALIO_TOUCH_GT911 */

  write(1, "GPS0\n", 5);
#ifdef CONFIG_ESPRESSIF_UART0
  /* GPS UART — power on the GNSS module and start the NMEA reader.  UART0
   * is registered as /dev/ttyS0 because the console is the USB serial. */
  ret = metalio_gps_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "GPS init failed: %d\n", ret);
    }
#endif
  write(1, "GPS1\n", 5);

#ifdef CONFIG_ESPRESSIF_I2S
  write(1, "BT0\n", 4);
  /* I2S audio — register /dev/audio/pcm0 (TX) + /dev/audio/pcm_in0 (RX).
   * On MetalioClaw4 the audio data path goes through the Bluetooth audio
   * codec chip (I2S master).  Power it on and send the AT sequence to enter
   * I2S audio mode (metalio_bt_audio_initialize), then route the PA to the
   * Wi-Fi / local audio path (PA_SWITCH) before starting I2S, matching the
   * reference METALIO_CLAW_4::InitializeIOExpander() / InitializeBTAudio(). */
  metalio_bt_audio_initialize();
  write(1, "BT1\n", 4);
  metalio_tca9555_write_pin(BOARD_IOEXP_PA_SWITCH, true);

  ret = board_i2s_init();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "I2S audio init failed: %d\n", ret);
    }
  else
    {
      /* Enable speaker PA; /dev/audio/pcm0 + /dev/audio/pcm_in0 are now
       * ready for the application-layer MetalioAudioCodec. */
      metalio_tca9555_write_pin(BOARD_IOEXP_PA_ENABLE, true);
      syslog(LOG_INFO, "I2S+PA ready\n");
    }
#endif /* CONFIG_ESPRESSIF_I2S */

#ifdef CONFIG_ESP32P4_SDMMC
  /* SD card — register /dev/mmcsd0 for subsequent FAT mount. */
  ret = metalio_sdcard_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "SD card init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_METALIO_ESP_HOSTED
  /* ESP-Hosted C5 over SDMMC Slot1.  This resets the C5, routes the Slot1
   * signals through the GPIO matrix and probes the slave with SDIO CMD5.
   * It does not reset the shared SDMMC controller (owned by Slot0/SD card)
   * and does not touch the USB CDC ACM console.
   */

  ret = metalio_esp_hosted_initialize();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "ESP-Hosted init failed: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "ESP-Hosted Wi-Fi host initialized\n");
    }
#endif

#ifdef CONFIG_ESPRESSIF_LEDC
  /* LCD backlight PWM — registers /dev/pwm0 (LEDC timer0, GPIO 52). */

  ret = board_ledc_setup();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "LEDC backlight init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_METALIO_CAMERA_OV2710
  /* Temporary V4L2 capture bring-up test: stream a few RAW10 frames from
   * /dev/video0 and report the result. */
#if 0
  ret = metalio_camera_capture_test();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "Camera capture test failed: %d\n", ret);
    }
#endif
#endif

  UNUSED(i2c);
  write(1, "BRINGUP_END\n", 12);
  return OK;
}
