/****************************************************************************
 * metalio/kvflash.c — tiny SPI-flash KV blob for Settings persistence
 *
 * NuttX on this board has no writable FS (/tmp is RAM).  Store a single
 * 4 KiB sector at a fixed high flash offset (past the ~8 MiB app image)
 * using ROM SPI-flash helpers already linked into the image.
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include <metalio/metalio.h>

/* ROM SPI flash — already linked; avoid pulling esp-hal headers into board. */
typedef enum
{
  ESP_ROM_SPIFLASH_RESULT_OK,
  ESP_ROM_SPIFLASH_RESULT_ERR,
  ESP_ROM_SPIFLASH_RESULT_TIMEOUT
} esp_rom_spiflash_result_t;

esp_rom_spiflash_result_t esp_rom_spiflash_unlock(void);
esp_rom_spiflash_result_t esp_rom_spiflash_erase_sector(uint32_t sector_num);
esp_rom_spiflash_result_t esp_rom_spiflash_write(uint32_t dest_addr,
                                                 const uint32_t *src,
                                                 int32_t len);
esp_rom_spiflash_result_t esp_rom_spiflash_read(uint32_t src_addr,
                                                uint32_t *dest,
                                                int32_t len);

/* 32 MiB flash: keep clear of nuttx.bin at 0x2000 (~8 MiB used). */
#ifndef METALIO_KVFLASH_ADDR
#  define METALIO_KVFLASH_ADDR  0x01f00000u
#endif

#ifndef METALIO_KVFLASH_SIZE
#  define METALIO_KVFLASH_SIZE  4096u
#endif

#define METALIO_KVFLASH_MAGIC   0x314f494du /* 'MIO1' little-endian */

struct kvflash_hdr_s
{
  uint32_t magic;
  uint32_t length;   /* payload bytes following this header */
  uint32_t checksum; /* sum of payload bytes */
};

/****************************************************************************/

static uint32_t kv_checksum(FAR const uint8_t *data, uint32_t len)
{
  uint32_t sum = 0;
  uint32_t i;

  for (i = 0; i < len; i++)
    {
      sum = (sum + data[i]) + (sum << 1);
    }

  return sum;
}

int metalio_kvflash_read(FAR void *buf, size_t buflen)
{
  uint8_t sector[METALIO_KVFLASH_SIZE] __attribute__((aligned(4)));
  struct kvflash_hdr_s *hdr = (FAR struct kvflash_hdr_s *)sector;
  irqstate_t flags;
  esp_rom_spiflash_result_t rr;
  uint32_t pay_len;

  if (buf == NULL || buflen == 0)
    {
      return -EINVAL;
    }

  memset(sector, 0xff, sizeof(sector));
  flags = up_irq_save();
  rr = esp_rom_spiflash_read(METALIO_KVFLASH_ADDR,
                             (uint32_t *)sector,
                             (int32_t)METALIO_KVFLASH_SIZE);
  up_irq_restore(flags);
  if (rr != ESP_ROM_SPIFLASH_RESULT_OK)
    {
      syslog(LOG_ERR, "kvflash: read failed %d\n", (int)rr);
      return -EIO;
    }

  if (hdr->magic != METALIO_KVFLASH_MAGIC)
    {
      return -ENOENT;
    }

  pay_len = hdr->length;
  if (pay_len == 0 ||
      pay_len > METALIO_KVFLASH_SIZE - sizeof(*hdr) ||
      pay_len > buflen)
    {
      return -EIO;
    }

  if (kv_checksum(sector + sizeof(*hdr), pay_len) != hdr->checksum)
    {
      syslog(LOG_ERR, "kvflash: checksum mismatch\n");
      return -EIO;
    }

  memcpy(buf, sector + sizeof(*hdr), pay_len);
  return (int)pay_len;
}

int metalio_kvflash_write(FAR const void *buf, size_t len)
{
  uint8_t sector[METALIO_KVFLASH_SIZE] __attribute__((aligned(4)));
  struct kvflash_hdr_s *hdr = (FAR struct kvflash_hdr_s *)sector;
  irqstate_t flags;
  esp_rom_spiflash_result_t rr;
  uint32_t sector_num;
  uint32_t write_len;

  if (buf == NULL || len == 0 ||
      len > METALIO_KVFLASH_SIZE - sizeof(*hdr))
    {
      return -EINVAL;
    }

  memset(sector, 0xff, sizeof(sector));
  hdr->magic = METALIO_KVFLASH_MAGIC;
  hdr->length = (uint32_t)len;
  memcpy(sector + sizeof(*hdr), buf, len);
  hdr->checksum = kv_checksum(sector + sizeof(*hdr), (uint32_t)len);

  /* ROM write requires 4-byte length. */
  write_len = (uint32_t)((sizeof(*hdr) + len + 3u) & ~3u);
  if (write_len > METALIO_KVFLASH_SIZE)
    {
      write_len = METALIO_KVFLASH_SIZE;
    }

  sector_num = METALIO_KVFLASH_ADDR / METALIO_KVFLASH_SIZE;

  flags = up_irq_save();
  (void)esp_rom_spiflash_unlock();
  rr = esp_rom_spiflash_erase_sector(sector_num);
  if (rr == ESP_ROM_SPIFLASH_RESULT_OK)
    {
      rr = esp_rom_spiflash_write(METALIO_KVFLASH_ADDR,
                                  (const uint32_t *)sector,
                                  (int32_t)write_len);
    }

  up_irq_restore(flags);

  if (rr != ESP_ROM_SPIFLASH_RESULT_OK)
    {
      syslog(LOG_ERR, "kvflash: write failed %d\n", (int)rr);
      return -EIO;
    }

  syslog(LOG_INFO, "kvflash: saved %u bytes @0x%08x\n",
         (unsigned)len, (unsigned)METALIO_KVFLASH_ADDR);
  return OK;
}
