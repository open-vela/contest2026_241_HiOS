/* SPDX-License-Identifier: Apache-2.0
 * Two committed settings slots. A power loss while programming the inactive
 * slot never erases the previous committed copy. Payload remains MIO1, including
 * the original 4084-byte maximum; journal metadata uses a separate sector.
 */
#include <nuttx/mutex.h>
#include <metalio/flash_layout.h>

int esp_flash32_read(uint32_t, void *, size_t);
int esp_flash32_write(uint32_t, const void *, size_t);
int esp_flash32_erase(uint32_t);

#define KV_MAGIC 0x314f494du
#define JOURNAL_MAGIC 0x324a564bu
#define JOURNAL_COMMIT 0x324b5643u
struct kv32_header { uint32_t magic, length, checksum; };
struct kv32_meta
{
  uint32_t magic, generation, length, checksum, header_checksum, commit;
};
static mutex_t g_kv32_lock = NXMUTEX_INITIALIZER;
static const uint32_t g_kv32_data[2] =
  { METALIO_SETTINGS_A, METALIO_SETTINGS_B };
static const uint32_t g_kv32_meta[2] =
  { METALIO_SETTINGS_A_META, METALIO_SETTINGS_B_META };

static uint32_t kv32_checksum(const void *data, size_t length)
{
  const uint8_t *p = data; uint32_t sum = 0;
  while (length--) sum = sum * 3u + *p++;
  return sum;
}

static bool kv32_payload_valid(uint8_t *sector)
{
  struct kv32_header *h = (void *)sector;
  return h->magic == KV_MAGIC && h->length &&
         h->length <= METALIO_FLASH_SECTOR - sizeof(*h) &&
         h->checksum == kv32_checksum(sector + sizeof(*h), h->length);
}

static int kv32_load(unsigned slot, struct kv32_meta *m, uint8_t *sector)
{
  int rc = esp_flash32_read(g_kv32_meta[slot], m, sizeof(*m));
  if (rc < 0) return rc;
  if (m->magic != JOURNAL_MAGIC || m->commit != JOURNAL_COMMIT ||
      m->header_checksum != kv32_checksum(m, 16)) return -ENOENT;
  rc = esp_flash32_read(g_kv32_data[slot], sector, METALIO_FLASH_SECTOR);
  if (rc < 0) return rc;
  struct kv32_header *h = (void *)sector;
  if (!kv32_payload_valid(sector) || m->length != h->length ||
      m->checksum != h->checksum) return -ENOENT;
  return 0;
}

static bool kv32_newer(uint32_t a, uint32_t b)
{
  return (int32_t)(a - b) > 0;
}

int metalio_kvflash_read(void *buf, size_t buflen)
{
  if (!buf || !buflen) return -EINVAL;
  int rc = nxmutex_lock(&g_kv32_lock);
  if (rc < 0) return rc;
  uint8_t sector[METALIO_FLASH_SECTOR] __attribute__((aligned(4)));
  struct kv32_meta m; uint32_t generation = 0;
  bool found = false; int result = -ENOENT;
  for (unsigned slot = 0; slot < 2; slot++)
    {
      rc = kv32_load(slot, &m, sector);
      if (rc < 0) { if (rc != -ENOENT && !found) result = rc; continue; }
      if (found && !kv32_newer(m.generation, generation)) continue;
      generation = m.generation; found = true;
      if (m.length > buflen) { result = -EIO; continue; }
      memcpy(buf, sector + sizeof(struct kv32_header), m.length);
      result = m.length;
    }
  nxmutex_unlock(&g_kv32_lock);
  return result;
}

int metalio_kvflash_write(const void *buf, size_t len)
{
  if (!buf || !len || len > METALIO_FLASH_SECTOR - sizeof(struct kv32_header))
    return -EINVAL;
  int rc = nxmutex_lock(&g_kv32_lock);
  if (rc < 0) return rc;
  uint8_t sector[METALIO_FLASH_SECTOR] __attribute__((aligned(4)));
  struct kv32_meta m, current; int active = -1;
  for (unsigned slot = 0; slot < 2; slot++)
    {
      rc = kv32_load(slot, &m, sector);
      if (!rc && (active < 0 || kv32_newer(m.generation, current.generation)))
        { active = slot; current = m; }
      else if (rc != 0 && rc != -ENOENT)
        { nxmutex_unlock(&g_kv32_lock); return rc; }
    }
  unsigned target = active == 0 ? 1 : 0;
  memset(sector, 0xff, sizeof(sector));
  struct kv32_header *h = (void *)sector;
  h->magic = KV_MAGIC; h->length = len;
  memcpy(sector + sizeof(*h), buf, len);
  h->checksum = kv32_checksum(buf, len);
  m.magic = JOURNAL_MAGIC;
  m.generation = active < 0 ? 1 : current.generation + 1;
  m.length = len; m.checksum = h->checksum;
  m.header_checksum = kv32_checksum(&m, 16); m.commit = JOURNAL_COMMIT;
  uint32_t expected_checksum = m.checksum, expected_generation = m.generation;
  rc = esp_flash32_erase(g_kv32_data[target]);
  if (!rc) rc = esp_flash32_write(g_kv32_data[target], sector, sizeof(sector));
  if (!rc) rc = esp_flash32_erase(g_kv32_meta[target]);
  if (!rc) rc = esp_flash32_write(g_kv32_meta[target], &m, sizeof(m));
  if (!rc) rc = kv32_load(target, &m, sector);
  if (!rc && (m.length != len || m.checksum != expected_checksum ||
              m.generation != expected_generation)) rc = -EIO;
  nxmutex_unlock(&g_kv32_lock);
  return rc;
}
