#include <nuttx/config.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <nuttx/mmcsd.h>
#include <nuttx/sdio.h>
#include <nuttx/fs/ioctl.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>
#include <stdbool.h>

extern struct sdio_dev_s *sdio_initialize(int slotno);

/* Shared SDMMC host clock recover — also used by Slot1 / ESP-Hosted. */
void esp32p4_sdmmc_host_rearm(void);

static bool g_sdcard_mounted;

bool metalio_sdcard_is_mounted(void)
{
  return g_sdcard_mounted;
}

static void metalio_sdcard_empty_recover(FAR struct sdio_dev_s *sdio)
{
  if (sdio != NULL && sdio->cancel != NULL)
    {
      sdio->cancel(sdio);
    }

  esp32p4_sdmmc_host_rearm();
  syslog(LOG_INFO,
         "SDMMC: empty-slot host rearm done (CLK=%d CMD=%d D0=%d)\n",
         BOARD_SDMMC_CLK_GPIO, BOARD_SDMMC_CMD_GPIO, BOARD_SDMMC_D0_GPIO);
}

static int metalio_sdcard_try_open(void)
{
  return open("/dev/mmcsd0", O_RDONLY);
}

/* Deferred RO mount — call after UI/Wi-Fi are up so Slot0 TX never races
 * bringup.  NuttX vfat still issues FAT-table writes on some paths even
 * with MS_RDONLY; keep mount off the critical boot path.
 */

int metalio_sdcard_mount_ro(void)
{
  int ret;

  if (g_sdcard_mounted)
    {
      return OK;
    }

  ret = mkdir("/sdcard", 0777);
  if (ret < 0 && errno != EEXIST)
    {
      return -errno;
    }

  ret = mount("/dev/mmcsd0", "/sdcard", "vfat", MS_RDONLY, NULL);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "SDMMC: deferred vfat RO mount failed errno=%d\n",
             errno);
      return -errno;
    }

  g_sdcard_mounted = true;
  syslog(LOG_INFO, "SDMMC: mounted /dev/mmcsd0 at /sdcard (vfat, ro)\n");
  return OK;
}

int metalio_sdcard_initialize(void)
{
  FAR struct sdio_dev_s *sdio;
  int ret;
  int fd = -1;
  int attempt;

  /* Enable SD power via TCA9555 (active low). */

  metalio_tca9555_write_pin(BOARD_IOEXP_SD_POWER, false);

  {
    bool pwr;
    if (metalio_tca9555_read_pin(BOARD_IOEXP_SD_POWER, &pwr) >= 0)
      {
        syslog(LOG_INFO, "SDMMC: SD_POWER pin level=%d (0=on)\n", pwr ? 1 : 0);
      }
  }

  usleep(300 * 1000);

  sdio = sdio_initialize(0);
  if (sdio == NULL)
    {
      syslog(LOG_ERR, "SDMMC: sdio_initialize failed\n");
      return -ENODEV;
    }

  /* Slot0 bring-up only: enumerate the card so the shared SDMMC PHY/CIU
   * is warm for Slot1 (ESP-Hosted).  Do NOT mount FAT here — any Slot0
   * FAT write (directory/FAT table) can wedge the controller and leave
   * the UI on the boot blue screen.
   */

  ret = mmcsd_slotinitialize(0, sdio);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "SDMMC: mmcsd_slotinitialize failed: %d "
             "(insert microSD for reliable C5 Wi-Fi)\n",
             ret);
      metalio_sdcard_empty_recover(sdio);
      return OK;
    }

  syslog(LOG_INFO, "SDMMC initialized (CLK=%d CMD=%d D0=%d)\n",
         BOARD_SDMMC_CLK_GPIO, BOARD_SDMMC_CMD_GPIO, BOARD_SDMMC_D0_GPIO);

  for (attempt = 0; attempt < 8; attempt++)
    {
      fd = metalio_sdcard_try_open();
      if (fd >= 0)
        {
          break;
        }

      syslog(LOG_INFO,
             "SDMMC: /dev/mmcsd0 not ready (errno=%d), re-probe %d/7\n",
             errno, attempt + 1);
      usleep(400 * 1000);
      esp32p4_sdmmc_host_rearm();
      SDIO_CALLBACKENABLE(sdio, SDIOMEDIA_INSERTED);
    }

  if (fd < 0)
    {
      syslog(LOG_WARNING,
             "SDMMC: open /dev/mmcsd0 failed: %d — insert FAT32 microSD for "
             "reliable C5 Wi-Fi SDIO\n",
             errno);
      metalio_sdcard_empty_recover(sdio);
      return OK;
    }

  {
    struct geometry geo;

    ret = ioctl(fd, BIOC_GEOMETRY, (unsigned long)&geo);
    close(fd);
    fd = -1;

    if (ret < 0)
      {
        syslog(LOG_WARNING, "SDMMC: BIOC_GEOMETRY failed: %d\n", errno);
        metalio_sdcard_empty_recover(sdio);
        return OK;
      }

    syslog(LOG_INFO,
           "SDMMC: /dev/mmcsd0 %lu sectors x %u bytes = %lu KB (%s)\n",
           (unsigned long)geo.geo_nsectors,
           (unsigned int)geo.geo_sectorsize,
           (unsigned long)(geo.geo_nsectors * geo.geo_sectorsize / 1024),
           geo.geo_model);
  }

  {
    uint8_t sector[512];

    fd = open("/dev/mmcsd0", O_RDONLY);
    if (fd >= 0)
      {
        ret = read(fd, sector, sizeof(sector));
        close(fd);
        if (ret == (int)sizeof(sector))
          {
            syslog(LOG_INFO,
                   "SDMMC: sector0 %02x %02x %02x %02x %02x %02x %02x %02x "
                   "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                   sector[0], sector[1], sector[2], sector[3],
                   sector[4], sector[5], sector[6], sector[7],
                   sector[8], sector[9], sector[10], sector[11],
                   sector[12], sector[13], sector[14], sector[15]);
          }
      }
  }

  mkdir("/sdcard", 0777); /* mountpoint placeholder; FAT mount deferred */
  syslog(LOG_INFO,
         "SDMMC: Slot0 ready (FAT mount deferred — avoid boot WR hang)\n");
  return OK;
}
