/****************************************************************************
 * Boot backlight claim + one-shot power-on vibrate.
 *
 * Backlight (/dev/pwm0, GPIO 52) must stay open. Closing it, or opening
 * the motor first, shuts down the shared LEDC block and blacks the panel.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/timers/pwm.h>

#include <arch/board/board.h>
#include <metalio/metalio.h>

#ifndef BACKLIGHT_DEFAULT_PERCENT
#  define BACKLIGHT_DEFAULT_PERCENT 75
#endif

/* Match the backlight timer. 5 kHz selects a different LEDC global
 * clock and the panel flickers for the whole pulse. */
#ifndef BACKLIGHT_FREQ_HZ
#  define BACKLIGHT_FREQ_HZ 25000
#endif

static int g_bl_fd = -1;
static int g_motor_fd = -1;
static bool g_bl_started;
static bool g_vibrated;

static int pwm_set_duty(int fd, uint32_t freq_hz, uint8_t pct, bool *started)
{
  struct pwm_info_s info;

  if (fd < 0)
    {
      return -ENODEV;
    }

  if (pct > 100)
    {
      pct = 100;
    }

  memset(&info, 0, sizeof(info));
  info.frequency = freq_hz;
  info.duty = (uint32_t)((pct * 65535u) / 100u);
  info.cpol = PWM_CPOL_NDEF;
  info.dcpol = PWM_DCPOL_NDEF;

  if (ioctl(fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info) < 0)
    {
      return -errno;
    }

  if (started != NULL && !(*started))
    {
      if (ioctl(fd, PWMIOC_START, 0) < 0)
        {
          return -errno;
        }

      *started = true;
    }

  return OK;
}

int metalio_boot_claim_backlight_pwm(void)
{
  if (g_bl_fd >= 0)
    {
      return OK;
    }

  g_bl_fd = open("/dev/pwm0", O_WRONLY);
  if (g_bl_fd < 0)
    {
      write(1, "BL_OPEN_FAIL\n", 13);
      return -errno;
    }

  if (pwm_set_duty(g_bl_fd, 25000, BACKLIGHT_DEFAULT_PERCENT,
                   &g_bl_started) < 0)
    {
      write(1, "BL_PWM_FAIL\n", 12);
      return -EIO;
    }

  write(1, "BL_CLAIM\n", 9);
  return OK;
}

void metalio_boot_release_backlight_pwm(void)
{
  /* Intentionally keep /dev/pwm0 open for the life of the process. */
}

void metalio_boot_pwr_vibrate_once(void)
{
  static bool motor_started;

  if (g_vibrated)
    {
      return;
    }

  g_vibrated = true;

  /* Do not reconfigure /dev/pwm0. The app owns backlight duty and has
   * already flushed the boot guide before this runs. */
  if (g_motor_fd < 0)
    {
      g_motor_fd = open("/dev/pwm1", O_WRONLY);
    }

  if (g_motor_fd < 0)
    {
      write(1, "VIB_FAIL\n", 9);
      return;
    }

  write(1, "VIB0\n", 5);
  (void)pwm_set_duty(g_motor_fd, BACKLIGHT_FREQ_HZ, 80, &motor_started);
  usleep(180 * 1000);
  (void)pwm_set_duty(g_motor_fd, BACKLIGHT_FREQ_HZ, 0, &motor_started);
  write(1, "VIB1\n", 5);
}
