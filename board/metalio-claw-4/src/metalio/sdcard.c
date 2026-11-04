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
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/fs/fs.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>
#include <stdbool.h>

extern struct sdio_dev_s *sdio_initialize(int slotno);

/* Shared SDMMC host clock recover — also used by Slot1 / ESP-Hosted. */
void esp32p4_sdmmc_host_rearm(void);

static mutex_t g_sdcard_mount_lock = NXMUTEX_INITIALIZER;
static bool g_sdcard_mounted;
static bool g_sdcard_rw;
static struct inode *g_sdcard_export;

bool metalio_sdcard_is_mounted(void)
{
  nxmutex_lock(&g_sdcard_mount_lock);
  bool mounted = g_sdcard_mounted;
  nxmutex_unlock(&g_sdcard_mount_lock);
  return mounted;
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

static int sdcard_mount_ro_locked(void)
{
  int ret;

  if (g_sdcard_export != NULL) return -EBUSY;

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
  g_sdcard_rw = false;
  syslog(LOG_INFO, "SDMMC: mounted /dev/mmcsd0 at /sdcard (vfat, ro)\n");
  return OK;
}

/* Camera save needs a writable FAT.  Boot stays unmounted; mount on demand. */

static int sdcard_ensure_rw_locked(void)
{
  int ret;

  if (g_sdcard_export != NULL) return -EBUSY;

  if (g_sdcard_mounted && g_sdcard_rw)
    {
      return OK;
    }

  if (g_sdcard_mounted)
    {
      if (umount("/sdcard") < 0)
        {
          /* Open music/recording files may keep the volume busy. Preserve
           * the real mount state and let the caller retry after closing. */
          return -errno;
        }
      g_sdcard_mounted = false;
      g_sdcard_rw = false;
    }

  ret = mkdir("/sdcard", 0777);
  if (ret < 0 && errno != EEXIST)
    {
      return -errno;
    }

  ret = mount("/dev/mmcsd0", "/sdcard", "vfat", 0, NULL);
  if (ret < 0)
    {
      int saved_errno = errno;
      syslog(LOG_WARNING, "SDMMC: vfat RW mount failed errno=%d\n",
             saved_errno);
      /* A failed write upgrade must not also remove browsing/playback.
       * Preserve the write error, but restore a readable mount if possible. */
      (void)sdcard_mount_ro_locked();
      return -saved_errno;
    }

  g_sdcard_mounted = true;
  g_sdcard_rw = true;
  syslog(LOG_INFO, "SDMMC: mounted /dev/mmcsd0 at /sdcard (vfat, rw)\n");
  return OK;
}

/* Capacity must not scan the entire FAT while holding fs_lock: that
 * serialises every music open/read and ringtone list behind thousands of
 * sector reads. Read only geometry/BPB/FSINFO. An unknown cached free count
 * is explicitly reported as UINT64_MAX, never as a fabricated zero. */
static uint32_t sdcard_le(const uint8_t *p,int bytes)
{
  uint32_t value=0;for(int i=0;i<bytes;i++)value|=(uint32_t)p[i]<<(8*i);return value;
}
int metalio_sdcard_capacity(uint64_t *total,uint64_t *available)
{
  if(!total||!available)return -EINVAL;
  *total=0;*available=UINT64_MAX;
  nxmutex_lock(&g_sdcard_mount_lock);
  if(g_sdcard_export){nxmutex_unlock(&g_sdcard_mount_lock);return -EBUSY;}
  struct inode *card=NULL;struct geometry geo;uint8_t sector[512];
  int ret=open_blockdriver("/dev/mmcsd0",MS_RDONLY,&card);
  if(ret<0)goto out;
  ret=card->u.i_bops->geometry(card,&geo);
  if(ret<0||!geo.geo_available||geo.geo_sectorsize!=512){ret=-ENODEV;goto close;}
  *total=(uint64_t)geo.geo_nsectors*512;
  off_t first=0;
  if(card->u.i_bops->read(card,sector,0,1)!=1){ret=-EIO;goto close;}
  if(sdcard_le(sector+11,2)!=512){
    first=sdcard_le(sector+446+8,4);
    if(first<=0||first>=geo.geo_nsectors||card->u.i_bops->read(card,sector,first,1)!=1){ret=OK;goto close;}
  }
  unsigned cluster=sector[13],reserved=sdcard_le(sector+14,2),fats=sector[16];
  uint32_t sectors=sdcard_le(sector+19,2);if(!sectors)sectors=sdcard_le(sector+32,4);
  uint32_t fat=sdcard_le(sector+22,2);bool fat32=fat==0;if(fat32)fat=sdcard_le(sector+36,4);
  unsigned roots=sdcard_le(sector+17,2),info=sdcard_le(sector+48,2);
  uint64_t overhead=(uint64_t)reserved+(uint64_t)fat*fats+(roots*32+511)/512;
  if(sdcard_le(sector+11,2)==512&&cluster&&!(cluster&(cluster-1))&&fats&&reserved&&fat&&sectors>overhead&&
     (uint64_t)first+sectors<=(uint64_t)geo.geo_nsectors){
    uint32_t clusters=(sectors-overhead)/cluster;*total=(uint64_t)clusters*cluster*512;
    if(fat32&&info&&info<reserved&&card->u.i_bops->read(card,sector,first+info,1)==1&&
       sdcard_le(sector,4)==0x41615252&&sdcard_le(sector+484,4)==0x61417272&&sdcard_le(sector+508,4)==0xaa550000){
      uint32_t free_count=sdcard_le(sector+488,4);if(free_count<=clusters)*available=(uint64_t)free_count*cluster*512;
    }
  }
  ret=OK;
close:
  close_blockdriver(card);
out:
  nxmutex_unlock(&g_sdcard_mount_lock);return ret;
}

int metalio_sdcard_mount_ro(void)
{
  nxmutex_lock(&g_sdcard_mount_lock);
  int ret = sdcard_mount_ro_locked();
  nxmutex_unlock(&g_sdcard_mount_lock);
  return ret;
}

int metalio_sdcard_ensure_rw(void)
{
  nxmutex_lock(&g_sdcard_mount_lock);
  int ret = sdcard_ensure_rw_locked();
  nxmutex_unlock(&g_sdcard_mount_lock);
  return ret;
}

/* The filesystem and USB host must never own the FAT concurrently.  Unmount
 * refuses an open photo, recording or music stream; no forced unmount here.
 * Use sector operations rather than a byte seek (off_t may be only 32 bits).
 */
int metalio_sdcard_export_begin(uint32_t *blocks)
{
  struct geometry geo;
  int ret;
  nxmutex_lock(&g_sdcard_mount_lock);
  if (g_sdcard_export != NULL) { ret = -EBUSY; goto out; }
  if (g_sdcard_mounted && umount("/sdcard") < 0)
    { ret = -errno; goto out; }
  g_sdcard_mounted = false;
  g_sdcard_rw = false;
  ret = open_blockdriver("/dev/mmcsd0", 0, &g_sdcard_export);
  if (ret < 0) { g_sdcard_export = NULL; goto restore; }
  ret = g_sdcard_export->u.i_bops->geometry(g_sdcard_export, &geo);
  if (ret >= 0 && (!geo.geo_available || !geo.geo_writeenabled ||
                  geo.geo_sectorsize != 512 || geo.geo_nsectors == 0))
    ret = -ENODEV;
  if (ret < 0)
    {
      close_blockdriver(g_sdcard_export);
      g_sdcard_export = NULL;
      goto restore;
    }
  *blocks = geo.geo_nsectors;
  ret = OK;
  goto out;
restore:
  sdcard_mount_ro_locked();
out:
  nxmutex_unlock(&g_sdcard_mount_lock);
  return ret;
}

int metalio_sdcard_export_io(uint32_t sector, void *buffer,
                            uint32_t count, bool write_data)
{
  nxmutex_lock(&g_sdcard_mount_lock);
  int ret = -ENODEV;
  if (g_sdcard_export != NULL)
    {
      if (write_data)
        ret = g_sdcard_export->u.i_bops->write(g_sdcard_export,
                                             buffer, sector, count);
      else
        ret = g_sdcard_export->u.i_bops->read(g_sdcard_export,
                                            buffer, sector, count);
      if (ret >= 0) ret = ret == count ? OK : -EIO;
    }
  nxmutex_unlock(&g_sdcard_mount_lock);
  return ret;
}

int metalio_sdcard_export_end(void)
{
  nxmutex_lock(&g_sdcard_mount_lock);
  int ret = OK;
  if (g_sdcard_export != NULL)
    {
      ret = close_blockdriver(g_sdcard_export);
      g_sdcard_export = NULL;
    }
  if (ret >= 0) ret = sdcard_mount_ro_locked();
  nxmutex_unlock(&g_sdcard_mount_lock);
  return ret;
}

int metalio_sdcard_initialize(void)
{
  FAR struct sdio_dev_s *sdio;
  int ret;
  int fd = -1;
  int attempt;

  /* The MCU can reboot while the separately powered card is still busy
   * after a failed transfer. Start this boot with a real card power cycle;
   * no filesystem has been mounted and USB does not own the card here. */

  ret = metalio_tca9555_write_pin(BOARD_IOEXP_SD_POWER, true);
  if (ret < 0) return ret;
  usleep(150 * 1000);
  ret = metalio_tca9555_write_pin(BOARD_IOEXP_SD_POWER, false);
  if (ret < 0) return ret;

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
