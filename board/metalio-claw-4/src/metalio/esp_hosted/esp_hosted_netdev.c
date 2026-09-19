/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/metalio/esp_hosted/esp_hosted_netdev.c
 *
 * ESP-Hosted host netdev for Metalio Claw4 (ESP32-P4 <-> ESP32-C5 SDIO)
 *
 * Transport pins (Slot1, 4-bit, RESET active-high):
 *   CMD=50 CLK=51 D0=49 D1=34 D2=31 D3=53 RESET=54
 *
 * The C5 runs the Espressif/Metalio slave firmware.  This host driver
 * implements the host side of the ESP SDIO slave protocol (see ESP-IDF
 * "Communication with ESP SDIO Slave") and exposes wlan0 to the NuttX
 * network stack.
 *
 * Data path notes:
 *   - Slot1 does not own the shared SDMMC interrupt, so the CMD53 data
 *     phase is driven by the polled esp32p4_sdio_rw_extended() helper in
 *     arch/risc-v/src/esp32p4/esp32p4_sdmmc.c.
 *   - Control-plane CMD52 operations (sdio_io_rw_direct / sdio_probe /
 *     sdio_set_blocksize / sdio_enable_function) are fully polled in the
 *     MMCSD layer and therefore safe for Slot1 as well.
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/param.h>

#include <nuttx/net/netdev.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/kmalloc.h>
#include <nuttx/clock.h>
#include <nuttx/wqueue.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/arch.h>
#include <debug.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <arch/board/board.h>
#include <metalio/metalio.h>

#include "espressif/esp_gpio.h"

/* The ESP32-P4 SDMMC lower-half exposes sdio_initialize(slotno).  There is
 * no shared arch header for this chip yet, so forward-declare both helpers
 * here (the SD card path in sdcard.c uses sdio_initialize the same way).
 */

FAR struct sdio_dev_s *sdio_initialize(int slotno);

int esp32p4_sdio_rw_extended(FAR struct sdio_dev_s *dev, bool write,
                             uint8_t function, uint32_t address,
                             bool inc_addr, FAR uint8_t *buf,
                             unsigned int blocklen, unsigned int nblocks);

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ESP SDIO slave register map (function 1), from the ESP-IDF "ESP SDIO
 * Slave Protocol" documentation.  These are the registers the host accesses
 * after enabling I/O function 1.
 */

#define ESP_SDIO_REG_TOKEN_RDATA   0x044u  /* RX buffer count, bits 27-16  */
#define ESP_SDIO_REG_INT_RAW       0x050u  /* slave -> host raw interrupt   */
#define ESP_SDIO_REG_INT_ST        0x058u  /* slave -> host interrupt bits  */
#define ESP_SDIO_REG_PKT_LEN       0x060u  /* accumulated TX data length    */
#define ESP_SDIO_REG_INT_CLR       0x0d4u  /* write 1 to clear INT_ST       */
#define ESP_SDIO_REG_INT_ENA       0x0dcu  /* slave -> host interrupt mask  */
#define ESP_SDIO_REG_SLAVE_INT     0x08cu  /* host -> slave interrupt       */
#define ESP_SDIO_INT_NEW_PACKET    (1u << 23) /* slave has a new RX packet  */
#define ESP_SDIO_FIFO_BASE         0x090u  /* FIFO start                    */
#define ESP_SDIO_FIFO_END          0x1f800u/* FIFO end (exclusive)          */

/* Host -> slave interrupt bit for opening the slave data path. */
#define ESP_SDIO_INT_OPEN_DATA_PATH 0x01u

#define ESP_SDIO_FUNC              1
#define ESP_SDIO_BLOCKLEN          512u

/* Streaming-mode RX bookkeeping (official esp-hosted H_SDIO_HOST_STREAMING_MODE).
 * The slave's PKT_LEN register (0x60) is a cumulative TX byte counter that
 * wraps at ESP_RX_BYTE_MAX; the host tracks how many bytes it has already
 * consumed in `rx_byte_count` and derives the bytes available to read with
 * the same wraparound arithmetic as sdio_get_len_from_slave().
 */
#define ESP_SLAVE_LEN_MASK         0xFFFFFu
#define ESP_RX_BYTE_MAX            0x100000u

/* Official esp-hosted SDIO transport framing (common/esp_hosted_header.h).
 *
 * Every FIFO transfer is:
 *   [ struct esp_payload_header (12 bytes) ][ payload ]
 *
 * All 16-bit header fields are little-endian on the wire.  `offset` is the
 * byte offset of the payload (always 12).  `checksum` is the byte-sum of the
 * whole frame with the checksum field itself treated as zero (upstream
 * compute_checksum()).  `if_type`/`if_num` share byte 0; the last byte is the
 * per-interface packet type (used by HCI/priv, zero for serial/STA).
 *
 * The RPC control path is carried on the ESP_SERIAL_IF interface, wrapped in
 * the protocomm_pserial TLV scheme, with a protobuf `Rpc` message inside.
 */

#define ESP_HOSTED_PAYLOAD_HDR_LEN 12u
#define ESP_HOSTED_MAX_PAYLOAD     1524u   /* 1536 SDIO buffer - 12 header  */

/* esp_hosted_if_type_t (common/esp_hosted_interface.h) */
#define ESP_HOSTED_IF_STA          1u
#define ESP_HOSTED_IF_SERIAL       3u
#define ESP_HOSTED_IF_PRIV         5u
#define ESP_HOSTED_IF_MAX          8u

/* Protobuf Rpc enum values (common/proto/esp_hosted_rpc.proto). */
#define RPC_MSG_TYPE_REQ           1u
#define RPC_MSG_TYPE_RESP          2u
#define RPC_MSG_TYPE_EVENT         3u

#define RPC_ID_WIFI_INIT           278u
#define RPC_ID_WIFI_START          280u
#define RPC_ID_WIFI_CONNECT        282u
#define RPC_ID_WIFI_DISCONNECT     283u
#define RPC_ID_WIFI_SET_CONFIG     284u
#define RPC_ID_WIFI_SCAN_START     286u
#define RPC_ID_WIFI_SCAN_GET_APNUM 288u
#define RPC_ID_WIFI_SCAN_GET_APREC 289u
#define RPC_ID_WIFI_SET_MODE       260u

#define RPC_ID_EVENT_STA_SCAN_DONE    774u
#define RPC_ID_EVENT_STA_CONNECTED    775u
#define RPC_ID_EVENT_STA_DISCONNECTED 776u
#define RPC_ID_EVENT_DHCP_DNS_STATUS  777u

#define RPC_ID_RESP_WIFI_SCAN_START     542u
#define RPC_ID_RESP_WIFI_SCAN_GET_APNUM 544u
#define RPC_ID_RESP_WIFI_SCAN_GET_APREC 545u

#define RPC_ID_REQ_GET_DHCP_DNS         353u
#define RPC_ID_RESP_GET_DHCP_DNS        609u

#define RPC_ID_REQ_GET_MAC              257u
#define RPC_ID_RESP_GET_MAC             513u

/* protocomm_pserial TLV framing carried on the serial interface. */
#define RPC_EP_NAME                "RPCRsp"
#define RPC_EP_NAME_EVT            "RPCEvt"
#define RPC_TLV_TYPE_EPNAME        0x01u
#define RPC_TLV_TYPE_DATA          0x02u

/* Protobuf nested field numbers used when composing wifi_config.sta.
 * `iface` uses ESP-IDF wifi_interface_t semantics (WIFI_IF_STA == 0), which is
 * unrelated to the esp_hosted_if_type_t values above.
 */
#define WIFI_IFACE_STA             0u
#define WIFI_MODE_STA              1u
#define WIFI_ALL_CHANNEL_SCAN      0u
#define WIFI_INIT_CONFIG_MAGIC     0x1f2f3f4fu
#define WIFI_CONFIG_STA_FIELD      2u

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct metalio_hosted_s
{
  struct netdev_lowerhalf_s dev;       /* Must be first */
  FAR struct sdio_dev_s *sdio;         /* SDMMC Slot1 lower-half */
  mutex_t lock;                        /* Protects RX state + FIFO access */
  mutex_t scan_lock;                   /* Serialises metalio_esp_hosted_scan() */
  struct work_s pollwork;              /* RX poll worker */
  bool ifup;
  bool link_up;                        /* SDIO card initialised, func1 ready */
  uint16_t tx_seq;                     /* esp_payload_header sequence number */
  uint32_t tx_pkts;                    /* STA Ethernet frames sent to slave   */
  uint32_t rx_pkts;                    /* STA Ethernet frames from slave      */
  uint32_t rx_byte_count;              /* Cumulative TX bytes consumed (wraps) */
  unsigned int rx_len;                 /* Bytes buffered but not yet parsed */
  unsigned int rx_cap;                 /* Allocated size of rx_buf */
  FAR uint8_t *rx_buf;                 /* Dynamically grown RX buffer */
  bool wifi_init;                      /* Req_WifiInit already sent          */
  bool mac_queried;                    /* STA MAC already fetched from slave  */
  volatile bool get_mac_resp;          /* Resp_GetMacAddress received         */
  uint8_t sta_mac[6];                  /* Slave's real STA MAC               */
  bool sta_connected;                  /* Last Event_StaConnected state       */
  char ssid[33];
  char pass[65];
  char conn_ssid[33];                  /* SSID reported by the connected AP  */
  char ip[16];                         /* DHCP-issued address (string)        */
  char nm[16];                         /* DHCP-issued netmask (string)        */
  char gw[16];                         /* DHCP-issued gateway (string)        */
  char dns[16];                        /* DNS server address (string)         */

  /* Wi-Fi scan state (written by the RX dispatcher, polled by the public
   * scan API running in a caller task).
   */
  volatile bool scan_start_resp;
  volatile bool scan_done_event;
  volatile bool scan_apnum_resp;
  volatile bool scan_aprec_resp;
  volatile int scan_start_status;      /* esp_err_t echoed by Resp_WifiScanStart */
  volatile uint32_t scan_done_number;
  volatile uint32_t scan_apnum_number;
  uint16_t scan_ap_count;
  struct metalio_wifi_ap_s scan_aps[METALIO_WIFI_MAX_AP];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct metalio_hosted_s *g_hosted;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Serialise the 12-byte esp_payload_header into buf.  The payload must already
 * be present at buf + ESP_HOSTED_PAYLOAD_HDR_LEN so the checksum can cover the
 * whole frame.
 */

static void hosted_frame_put_hdr(FAR uint8_t *buf, uint8_t if_type,
                                 uint16_t payload_len, uint16_t seq)
{
  uint16_t csum = 0;
  uint16_t total = ESP_HOSTED_PAYLOAD_HDR_LEN + payload_len;
  int i;

  buf[0] = if_type & 0x0f;                     /* if_num = 0               */
  buf[1] = 0;                                  /* flags                    */
  buf[2] = payload_len & 0xff;
  buf[3] = (payload_len >> 8) & 0xff;
  buf[4] = ESP_HOSTED_PAYLOAD_HDR_LEN & 0xff;  /* offset                   */
  buf[5] = (ESP_HOSTED_PAYLOAD_HDR_LEN >> 8) & 0xff;
  buf[6] = 0;                                  /* checksum (filled below)  */
  buf[7] = 0;
  buf[8] = seq & 0xff;
  buf[9] = (seq >> 8) & 0xff;
  buf[10] = 0;                                 /* throttle_cmd/reserved2   */
  buf[11] = 0;                                 /* reserved3/priv_pkt_type  */

  for (i = 0; i < total; i++)
    {
      csum += buf[i];
    }

  buf[6] = csum & 0xff;
  buf[7] = (csum >> 8) & 0xff;
}

/* Read a 32-bit slave register through four CMD52 (8-bit) reads. */

static int hosted_reg_read32(FAR struct metalio_hosted_s *priv,
                             uint32_t addr, FAR uint32_t *val)
{
  uint8_t b[4];
  int ret;
  int i;

  for (i = 0; i < 4; i++)
    {
      ret = sdio_io_rw_direct(priv->sdio, false, ESP_SDIO_FUNC,
                              addr + i, 0, &b[i]);
      if (ret < 0)
        {
          return ret;
        }
    }

  *val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return OK;
}

/* Clear the slave->host interrupt bits by writing 1 to INT_CLR (0xd4), the
 * same way the official sdio_clear_intr() does.  This is required so a
 * consumed NEW_PACKET interrupt does not keep the slave's interrupt line
 * asserted (and to keep the host's polling snapshot consistent with the
 * official read task).
 */

static int hosted_clear_slave_intr(FAR struct metalio_hosted_s *priv,
                                   uint32_t mask)
{
  uint8_t buf[4];

  /* The ESP SDIO slave's INT_CLR register (0xd4) is write-1-to-clear and
   * must be written in a single 4-byte CMD53 byte-mode transaction, exactly
   * like the official sdio_clear_intr() -> sdmmc_io_write_bytes(..., 4).
   *
   * Writing it as four separate CMD52 byte transfers only clears NEW_PACKET
   * (bit23, as a side effect of the preceding FIFO read) and leaves the
   * RX_SOF/RX_EOF/RX_START/EXT_BIT3 bits (12-22) latched, which stops the
   * slave from ever asserting a fresh NEW_PACKET and stalls RX after the
   * first packet.
   */

  buf[0] = (uint8_t)(mask & 0xff);
  buf[1] = (uint8_t)((mask >> 8) & 0xff);
  buf[2] = (uint8_t)((mask >> 16) & 0xff);
  buf[3] = (uint8_t)((mask >> 24) & 0xff);

  return esp32p4_sdio_rw_extended(priv->sdio, true, ESP_SDIO_FUNC,
                                  ESP_SDIO_REG_INT_CLR, true, buf, 4, 0);
}

/* Transfer len bytes to/from the slave FIFO using CMD53.  The CMD53 address
 * encodes the requested length as (0x1F800 - address), per the ESP SDIO
 * slave protocol.
 *
 * The Espressif reference enables H_SDIO_TX_BLOCK_ONLY_XFER /
 * H_SDIO_RX_BLOCK_ONLY_XFER: it always uses block-mode CMD53 and pads the
 * transfer up to a 512-byte boundary, letting the slave ignore (TX) or zero
 * fill (RX) the padding.  Byte-mode CMD53 is unreliable with the P4 SDMMC
 * IDMAC, so follow the same block-only policy here.
 */

static int hosted_fifo_transfer(FAR struct metalio_hosted_s *priv,
                                bool write, FAR uint8_t *buf,
                                unsigned int len)
{
  unsigned int padded;
  unsigned int nblocks;
  uint8_t *b = buf;
  int ret;

  if (len == 0)
    {
      return OK;
    }

  padded = (len + ESP_SDIO_BLOCKLEN - 1) & ~(ESP_SDIO_BLOCKLEN - 1);
  nblocks = padded / ESP_SDIO_BLOCKLEN;

  if (padded != len)
    {
      /* Pad with a bounce buffer.  For writes the padding is zero-filled
       * (slave ignores it); for reads it is discarded after the transfer.
       */

      b = kmm_zalloc(padded);
      if (b == NULL)
        {
          return -ENOMEM;
        }

      if (write)
        {
          memcpy(b, buf, len);
        }
    }

  ret = esp32p4_sdio_rw_extended(priv->sdio, write, ESP_SDIO_FUNC,
                                 ESP_SDIO_FIFO_END - len, true, b,
                                 ESP_SDIO_BLOCKLEN, nblocks);

  /* The C5 slave occasionally rejects the first data-plane block write right
   * after STA association (R5 error/out-of-range) while its Wi-Fi TX path is
   * still coming up.  A single transient SDIO error would make the NuttX DHCP
   * client abort on the very first DISCOVER, leaving the interface without an
   * IP (and DNS would then hang).  Retry the transient errors for a bounded
   * window instead of failing immediately.
   */

  if (ret == -EIO || ret == -EINVAL || ret == -ETIMEDOUT)
    {
      int attempt;

      for (attempt = 0; attempt < 20; attempt++)
        {
          usleep(10 * 1000);
          ret = esp32p4_sdio_rw_extended(priv->sdio, write, ESP_SDIO_FUNC,
                                         ESP_SDIO_FIFO_END - len, true, b,
                                         ESP_SDIO_BLOCKLEN, nblocks);
          if (ret == OK)
            {
              syslog(LOG_INFO,
                     "esp-hosted: fifo %s recovered after %d retry\n",
                     write ? "write" : "read", attempt + 1);
              break;
            }
        }
    }

  if (b != buf)
    {
      if (!write && ret == OK)
        {
          memcpy(buf, b, len);
        }

      kmm_free(b);
    }

  return ret;
}

static int hosted_fifo_write(FAR struct metalio_hosted_s *priv,
                             FAR const uint8_t *buf, unsigned int len)
{
  /* esp32p4_sdio_rw_extended() copies the source into its own bounce buffer,
   * so a non-const view is safe here even though the data is not modified.
   */

  return hosted_fifo_transfer(priv, true, (FAR uint8_t *)buf, len);
}

static int hosted_fifo_read(FAR struct metalio_hosted_s *priv,
                            FAR uint8_t *buf, unsigned int len)
{
  return hosted_fifo_transfer(priv, false, buf, len);
}

/* Minimal protobuf (proto3 wire) encoder for the Rpc control messages.  Only
 * the small subset needed to drive Wi-Fi connect is implemented: varint,
 * length-delimited (bytes/message) and enum fields.
 */

#define PB_BUF_CAP 128

struct pb_buf_s
{
  uint8_t data[PB_BUF_CAP];
  uint16_t len;
};

static void pb_varint(FAR struct pb_buf_s *b, uint32_t v)
{
  while (v >= 0x80)
    {
      b->data[b->len++] = (uint8_t)(v & 0x7f) | 0x80;
      v >>= 7;
    }

  b->data[b->len++] = (uint8_t)v;
}

static void pb_tag(FAR struct pb_buf_s *b, uint32_t field, uint8_t wire)
{
  pb_varint(b, (field << 3) | wire);
}

static void pb_enum(FAR struct pb_buf_s *b, uint32_t field, uint32_t value)
{
  pb_tag(b, field, 0);               /* wire type 0 = varint */
  pb_varint(b, value);
}

static void pb_uint32(FAR struct pb_buf_s *b, uint32_t field, uint32_t value)
{
  pb_tag(b, field, 0);
  pb_varint(b, value);
}

static void pb_bytes(FAR struct pb_buf_s *b, uint32_t field,
                     FAR const void *data, uint16_t len)
{
  pb_tag(b, field, 2);               /* wire type 2 = length-delimited */
  pb_varint(b, len);
  if (len > 0)
    {
      memcpy(&b->data[b->len], data, len);
      b->len += len;
    }
}

/* Compose the protobuf `Rpc` message for Req_WifiSetConfig(284):
 *   Rpc { msg_type=Req, msg_id=284, req_wifi_set_config {
 *           iface=STA, cfg { sta { ssid, password } } } }
 * Returns the encoded length, or 0 if it does not fit in out.
 */

static uint16_t hosted_rpc_wifi_set_config(FAR uint8_t *out, uint16_t cap,
                                           FAR const char *ssid,
                                           FAR const char *pass)
{
  struct pb_buf_s sta = {0};
  struct pb_buf_s wcfg = {0};
  struct pb_buf_s req = {0};
  struct pb_buf_s rpc = {0};
  uint8_t ssid_len = (uint8_t)MIN(strlen(ssid), 32);
  uint8_t pass_len = (uint8_t)MIN(strlen(pass ? pass : ""), 64);

  /* wifi_sta_config: ssid=1, password=2, scan_method=3 (all-channel),
   * failure_retry_cnt=13.  Matches the MetalioClaw4 reference which sets
   * scan_method = WIFI_ALL_CHANNEL_SCAN and failure_retry_cnt = 1.
   */
  pb_bytes(&sta, 1, ssid, ssid_len);
  pb_bytes(&sta, 2, pass ? pass : "", pass_len);
  pb_uint32(&sta, 3, WIFI_ALL_CHANNEL_SCAN);
  pb_uint32(&sta, 13, 1);

  /* wifi_config: sta=2 */
  pb_bytes(&wcfg, WIFI_CONFIG_STA_FIELD, sta.data, sta.len);

  /* Rpc_Req_WifiSetConfig: iface=1, cfg=2 */
  pb_uint32(&req, 1, WIFI_IFACE_STA);
  pb_bytes(&req, 2, wcfg.data, wcfg.len);

  /* Rpc: msg_type=1(Req), msg_id=2(284), payload field 284 */
  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_SET_CONFIG);
  pb_bytes(&rpc, RPC_ID_WIFI_SET_CONFIG, req.data, req.len);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose the protobuf `Rpc` message for Req_WifiInit(278).
 *
 * The slave's req_wifi_init()/get_merged_init_config() take the buffer
 * counts, AMPDU flags, RX BA window, etc. from the HOST request, not from the
 * slave's own defaults.  Sending only `magic` therefore leaves
 * static_rx_buf_num/dynamic_rx_buf_num/dynamic_tx_buf_num == 0 and the C5
 * hangs inside esp_wifi_init(), which is why no RPC response ever came back.
 *
 * Send the full WIFI_INIT_CONFIG_DEFAULT() of the reference host
 * (MetalioClaw4 P4 sdkconfig) with nvs_enable=false, mirroring the
 * esp_hosted 2.12.3 + esp_wifi_remote host stack.
 */

static uint16_t hosted_rpc_wifi_init(FAR uint8_t *out, uint16_t cap)
{
  struct pb_buf_s cfg = {0};
  struct pb_buf_s req = {0};
  struct pb_buf_s rpc = {0};

  /* wifi_init_config fields (proto numbers), from the reference P4 host:
   *   static_rx_buf_num=16  dynamic_rx_buf_num=32  tx_buf_type=0(static)
   *   static_tx_buf_num=16  cache_tx_buf_num=32    rx_mgmt_buf_num=5
   *   ampdu_rx=1 ampdu_tx=1 rx_ba_win=16 beacon_max_len=752 mgmt_sbuf_num=32
   *   feature_caps=0x1a3 (WPA3_SAE|CACHE_TX|GMAC|ENTERPRISE|BSS_MAX_IDLE)
   *   sta_disconnected_pm=1 espnow_max_encrypt_num=7 tx_hetb_queue_num=3
   *   magic=0x1f2f3f4f
   * Zero-valued fields are omitted (proto3 defaults to 0).
   */
  pb_uint32(&cfg, 1, 16);                    /* static_rx_buf_num      */
  pb_uint32(&cfg, 2, 32);                    /* dynamic_rx_buf_num     */
  pb_uint32(&cfg, 4, 16);                    /* static_tx_buf_num      */
  pb_uint32(&cfg, 6, 32);                    /* cache_tx_buf_num       */
  pb_uint32(&cfg, 8, 1);                     /* ampdu_rx_enable        */
  pb_uint32(&cfg, 9, 1);                     /* ampdu_tx_enable        */
  pb_uint32(&cfg, 13, 16);                   /* rx_ba_win              */
  pb_uint32(&cfg, 15, 752);                  /* beacon_max_len         */
  pb_uint32(&cfg, 16, 32);                   /* mgmt_sbuf_num          */
  pb_uint32(&cfg, 17, 419);                  /* feature_caps           */
  pb_uint32(&cfg, 18, 1);                    /* sta_disconnected_pm    */
  pb_uint32(&cfg, 19, 7);                    /* espnow_max_encrypt_num */
  pb_uint32(&cfg, 20, WIFI_INIT_CONFIG_MAGIC); /* magic                */
  pb_uint32(&cfg, 22, 5);                    /* rx_mgmt_buf_num        */
  pb_uint32(&cfg, 23, 3);                    /* tx_hetb_queue_num      */

  /* Rpc_Req_WifiInit: cfg=1 */
  pb_bytes(&req, 1, cfg.data, cfg.len);

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_INIT);
  pb_bytes(&rpc, RPC_ID_WIFI_INIT, req.data, req.len);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose the protobuf `Rpc` message for Req_WifiSetMode(260), selecting STA
 * mode (WIFI_MODE_STA == 1).
 */

static uint16_t hosted_rpc_wifi_set_mode(FAR uint8_t *out, uint16_t cap)
{
  struct pb_buf_s req = {0};
  struct pb_buf_s rpc = {0};

  pb_uint32(&req, 1, WIFI_MODE_STA);

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_SET_MODE);
  pb_bytes(&rpc, RPC_ID_WIFI_SET_MODE, req.data, req.len);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose the protobuf `Rpc` message for Req_WifiStart(280), which has an
 * empty payload.
 */

static uint16_t hosted_rpc_wifi_start(FAR uint8_t *out, uint16_t cap)
{
  struct pb_buf_s rpc = {0};

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_START);
  pb_bytes(&rpc, RPC_ID_WIFI_START, NULL, 0);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose the protobuf `Rpc` message for Req_WifiConnect(282), which has an
 * empty payload.  Returns the encoded length, or 0 on overflow.
 */

static uint16_t hosted_rpc_wifi_connect(FAR uint8_t *out, uint16_t cap)
{
  struct pb_buf_s rpc = {0};

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_CONNECT);
  pb_bytes(&rpc, RPC_ID_WIFI_CONNECT, NULL, 0);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose Req_WifiDisconnect(283) — empty payload. */

static uint16_t hosted_rpc_wifi_disconnect(FAR uint8_t *out, uint16_t cap)
{
  struct pb_buf_s rpc = {0};

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_DISCONNECT);
  pb_bytes(&rpc, RPC_ID_WIFI_DISCONNECT, NULL, 0);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose the protobuf `Rpc` message for Req_WifiGetMac(257).  The slave
 * returns its real STA MAC (from eFuse) in Resp_WifiGetMac(513); the host
 * netdev must use that same address so DHCP/ARP replies addressed to the
 * slave's 802.11 station MAC are accepted by the host Ethernet stack.
 */

static uint16_t hosted_rpc_wifi_get_mac(FAR uint8_t *out, uint16_t cap)
{
  struct pb_buf_s req = {0};
  struct pb_buf_s rpc = {0};

  /* Rpc_Req_GetMacAddress: mode=1 (wifi_interface_t, WIFI_IF_STA == 0). */
  pb_uint32(&req, 1, WIFI_IFACE_STA);

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_REQ_GET_MAC);
  pb_bytes(&rpc, RPC_ID_REQ_GET_MAC, req.data, req.len);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose the protobuf `Rpc` message for Req_WifiScanStart(286).  The
 * optional config/config_set fields are left unset so the slave runs a
 * default all-channel scan; `block` selects a blocking scan on the slave.
 */

static uint16_t hosted_rpc_wifi_scan_start(FAR uint8_t *out, uint16_t cap,
                                           bool block)
{
  struct pb_buf_s req = {0};
  struct pb_buf_s rpc = {0};

  pb_uint32(&req, 2, block ? 1u : 0u);

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_SCAN_START);
  pb_bytes(&rpc, RPC_ID_WIFI_SCAN_START, req.data, req.len);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose the protobuf `Rpc` message for Req_WifiScanGetApNum(288), which has
 * an empty payload.
 */

static uint16_t hosted_rpc_wifi_scan_get_apnum(FAR uint8_t *out, uint16_t cap)
{
  struct pb_buf_s rpc = {0};

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_SCAN_GET_APNUM);
  pb_bytes(&rpc, RPC_ID_WIFI_SCAN_GET_APNUM, NULL, 0);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Compose the protobuf `Rpc` message for Req_WifiScanGetApRecords(289),
 * requesting `number` records.
 */

static uint16_t hosted_rpc_wifi_scan_get_ap_records(FAR uint8_t *out,
                                                    uint16_t cap,
                                                    uint32_t number)
{
  struct pb_buf_s req = {0};
  struct pb_buf_s rpc = {0};

  pb_uint32(&req, 1, number);

  pb_enum(&rpc, 1, RPC_MSG_TYPE_REQ);
  pb_enum(&rpc, 2, RPC_ID_WIFI_SCAN_GET_APREC);
  pb_bytes(&rpc, RPC_ID_WIFI_SCAN_GET_APREC, req.data, req.len);

  if (rpc.len > cap)
    {
      return 0;
    }

  memcpy(out, rpc.data, rpc.len);
  return rpc.len;
}

/* Send a protobuf `Rpc` request over the serial interface.  The RPC message is
 * wrapped in the protocomm_pserial TLV scheme and then in an
 * esp_payload_header (if_type=ESP_SERIAL_IF) before hitting the FIFO.
 */

static int hosted_rpc_send(FAR struct metalio_hosted_s *priv,
                           FAR const uint8_t *rpc, uint16_t rpc_len)
{
  uint8_t *buf;
  uint8_t *p;
  uint16_t tlv_len;
  int ret;

  /* [0x01][ep_len:2]"RPCRsp"[0x02][data_len:2][rpc bytes] */
  tlv_len = 1 + 2 + strlen(RPC_EP_NAME) + 1 + 2 + rpc_len;

  buf = kmm_malloc(ESP_HOSTED_PAYLOAD_HDR_LEN + tlv_len);
  if (buf == NULL)
    {
      return -ENOMEM;
    }

  p = buf + ESP_HOSTED_PAYLOAD_HDR_LEN;
  *p++ = RPC_TLV_TYPE_EPNAME;
  *p++ = (uint8_t)strlen(RPC_EP_NAME);
  *p++ = 0;
  memcpy(p, RPC_EP_NAME, strlen(RPC_EP_NAME));
  p += strlen(RPC_EP_NAME);
  *p++ = RPC_TLV_TYPE_DATA;
  *p++ = rpc_len & 0xff;
  *p++ = (rpc_len >> 8) & 0xff;
  memcpy(p, rpc, rpc_len);

  hosted_frame_put_hdr(buf, ESP_HOSTED_IF_SERIAL, tlv_len, priv->tx_seq++);

  nxmutex_lock(&priv->lock);

  {
    uint32_t token = 0;

    /* Token read (CMD52) must be serialised with the RX poll worker's
     * INT_RAW/PKT_LEN CMD52 reads and with the FIFO CMD53 below; the P4
     * SDMMC controller is a single non-reentrant hardware block.
     */
    hosted_reg_read32(priv, ESP_SDIO_REG_TOKEN_RDATA, &token);
    (void)token;
  }

  ret = hosted_fifo_write(priv, buf, ESP_HOSTED_PAYLOAD_HDR_LEN + tlv_len);
  nxmutex_unlock(&priv->lock);

  kmm_free(buf);

  return ret;
}

/* Minimal protobuf (proto3 wire) decoder for the Rpc control responses and
 * events received over the serial interface.  Only the fields we consume are
 * decoded; unknown fields are skipped.
 */

struct pb_reader_s
{
  FAR const uint8_t *p;
  uint32_t len;
};

static bool pb_read_varint(FAR struct pb_reader_s *r, FAR uint32_t *val)
{
  uint32_t v = 0;
  unsigned int shift = 0;

  while (r->len > 0)
    {
      uint8_t b = *r->p++;
      r->len--;

      if (shift < 32)
        {
          v |= (uint32_t)(b & 0x7f) << shift;
        }

      if ((b & 0x80) == 0)
        {
          *val = v;
          return true;
        }

      shift += 7;
      if (shift >= 64)
        {
          return false;
        }
    }

  return false;
}

static bool pb_read_tag(FAR struct pb_reader_s *r, FAR uint32_t *field,
                        FAR uint8_t *wire)
{
  uint32_t key;

  if (!pb_read_varint(r, &key))
    {
      return false;
    }

  *field = key >> 3;
  *wire = key & 7;
  return true;
}

static bool pb_skip(FAR struct pb_reader_s *r, uint8_t wire)
{
  uint32_t v;
  uint32_t n;

  switch (wire)
    {
    case 0:
      return pb_read_varint(r, &v);

    case 1:
      if (r->len < 8)
        {
          return false;
        }

      r->p += 8;
      r->len -= 8;
      return true;

    case 2:
      if (!pb_read_varint(r, &n) || n > r->len)
        {
          return false;
        }

      r->p += n;
      r->len -= n;
      return true;

    case 5:
      if (r->len < 4)
        {
          return false;
        }

      r->p += 4;
      r->len -= 4;
      return true;

    default:
      return false;
    }
}

/* Return a length-delimited sub-field's value buffer and length. */

static bool pb_field_bytes(FAR const uint8_t *data, uint32_t len,
                           uint32_t want, FAR const uint8_t **out,
                           FAR uint32_t *out_len)
{
  struct pb_reader_s r;
  uint32_t field;
  uint8_t wire;

  r.p = data;
  r.len = len;

  while (pb_read_tag(&r, &field, &wire))
    {
      if (field == want && wire == 2)
        {
          if (!pb_read_varint(&r, out_len) || *out_len > r.len)
            {
              return false;
            }

          *out = r.p;
          return true;
        }

      if (!pb_skip(&r, wire))
        {
          return false;
        }
    }

  return false;
}

/* Return a varint sub-field's value. */

static bool pb_field_varint(FAR const uint8_t *data, uint32_t len,
                            uint32_t want, FAR uint32_t *out)
{
  struct pb_reader_s r;
  uint32_t field;
  uint8_t wire;

  r.p = data;
  r.len = len;

  while (pb_read_tag(&r, &field, &wire))
    {
      if (field == want && wire == 0)
        {
          return pb_read_varint(&r, out);
        }

      if (!pb_skip(&r, wire))
        {
          return false;
        }
    }

  return false;
}

/* Copy a protobuf `bytes` field as a NUL-terminated string into dst. */

static void hosted_copy_str_field(FAR char *dst, size_t dst_sz,
                                  FAR const uint8_t *data, uint32_t len,
                                  uint32_t field)
{
  FAR const uint8_t *s;
  uint32_t s_len = 0;

  dst[0] = '\0';
  if (pb_field_bytes(data, len, field, &s, &s_len))
    {
      uint32_t n = MIN(s_len, (uint32_t)(dst_sz - 1));

      memcpy(dst, s, n);
      dst[n] = '\0';
    }
}

/* RPC event/response handlers.  The payload for an event is the per-event
 * message (e.g. Rpc_Event_StaConnected), not the wrapping Rpc message.
 */

static void hosted_rpc_event_sta_connected(FAR struct metalio_hosted_s *priv,
                                           FAR const uint8_t *p, uint32_t len)
{
  FAR const uint8_t *sc;
  FAR const uint8_t *ssid;
  uint32_t sc_len;
  uint32_t ssid_len;
  uint32_t n;

  if (!pb_field_bytes(p, len, 2, &sc, &sc_len))
    {
      return;
    }

  if (pb_field_bytes(sc, sc_len, 1, &ssid, &ssid_len))
    {
      n = MIN(ssid_len, (uint32_t)(sizeof(priv->conn_ssid) - 1));
      memcpy(priv->conn_ssid, ssid, n);
      priv->conn_ssid[n] = '\0';
    }

  if (!priv->sta_connected)
    {
      netdev_lower_carrier_on(&priv->dev);
    }

  priv->sta_connected = true;
  syslog(LOG_INFO, "esp-hosted: STA connected SSID=%s\n", priv->conn_ssid);
}

static void hosted_rpc_event_sta_disconnected(FAR struct metalio_hosted_s *priv,
                                              FAR const uint8_t *p, uint32_t len)
{
  FAR const uint8_t *sd;
  uint32_t sd_len;
  uint32_t reason = 0;

  if (pb_field_bytes(p, len, 2, &sd, &sd_len))
    {
      pb_field_varint(sd, sd_len, 4, &reason);
    }

  if (priv->sta_connected)
    {
      netdev_lower_carrier_off(&priv->dev);
    }

  priv->sta_connected = false;
  syslog(LOG_INFO, "esp-hosted: STA disconnected reason=%lu\n",
         (unsigned long)reason);
}

static void hosted_rpc_event_sta_scan_done(FAR struct metalio_hosted_s *priv,
                                           FAR const uint8_t *p, uint32_t len)
{
  FAR const uint8_t *sd;
  uint32_t sd_len;
  uint32_t number = 0;

  if (pb_field_bytes(p, len, 2, &sd, &sd_len))
    {
      pb_field_varint(sd, sd_len, 2, &number);
    }

  priv->scan_done_number = number;
  priv->scan_done_event = true;

  syslog(LOG_INFO, "esp-hosted: scan done, %lu AP(s)\n",
         (unsigned long)number);
}

static void hosted_rpc_dhcp_dns_status(FAR struct metalio_hosted_s *priv,
                                       FAR const uint8_t *p, uint32_t len)
{
  uint32_t net_link_up = 0;
  uint32_t dhcp_up = 0;

  /* Rpc_Event_DhcpDnsStatus and Rpc_Resp_GetDhcpDnsStatus share the same
   * field layout: iface=1 net_link_up=2 dhcp_up=3 dhcp_ip=4 dhcp_nm=5
   * dhcp_gw=6 dns_up=7 dns_ip=8 dns_type=9 resp=10.  The address fields are
   * dotted-quad strings (e.g. "192.168.1.100"), not raw octets.
   */

  pb_field_varint(p, len, 2, &net_link_up);
  pb_field_varint(p, len, 3, &dhcp_up);

  hosted_copy_str_field(priv->ip,  sizeof(priv->ip),  p, len, 4);
  hosted_copy_str_field(priv->nm,  sizeof(priv->nm),  p, len, 5);
  hosted_copy_str_field(priv->gw,  sizeof(priv->gw),  p, len, 6);
  hosted_copy_str_field(priv->dns, sizeof(priv->dns), p, len, 8);

  syslog(LOG_INFO,
         "esp-hosted: DHCP link=%lu dhcp=%lu ip=%s nm=%s gw=%s dns=%s\n",
         (unsigned long)net_link_up, (unsigned long)dhcp_up,
         priv->ip, priv->nm, priv->gw, priv->dns);
}

static void hosted_rpc_event_dhcp_dns(FAR struct metalio_hosted_s *priv,
                                      FAR const uint8_t *p, uint32_t len)
{
  hosted_rpc_dhcp_dns_status(priv, p, len);
}

static void hosted_rpc_parse_ap_record(FAR struct metalio_hosted_s *priv,
                                       FAR const uint8_t *p, uint32_t len)
{
  FAR const uint8_t *ssid;
  uint32_t ssid_len = 0;
  uint32_t rssi = 0;
  uint32_t authmode = 0;
  uint32_t n;
  uint16_t idx = priv->scan_ap_count;

  if (idx >= METALIO_WIFI_MAX_AP)
    {
      return;
    }

  if (pb_field_bytes(p, len, 2, &ssid, &ssid_len))
    {
      n = MIN(ssid_len, (uint32_t)(sizeof(priv->scan_aps[idx].ssid) - 1));
      memcpy(priv->scan_aps[idx].ssid, ssid, n);
      priv->scan_aps[idx].ssid[n] = '\0';
    }
  else
    {
      priv->scan_aps[idx].ssid[0] = '\0';
    }

  pb_field_varint(p, len, 5, &rssi);
  pb_field_varint(p, len, 6, &authmode);

  /* rssi is a negative int32 encoded as a sign-extended 64-bit varint; the
   * low byte of the decoded value is the correct int8_t dBm.
   */

  priv->scan_aps[idx].rssi = (int8_t)(uint8_t)rssi;
  priv->scan_aps[idx].authmode = (int)authmode;

  priv->scan_ap_count++;
}

static void hosted_rpc_resp(FAR struct metalio_hosted_s *priv, uint32_t msg_id,
                            FAR const uint8_t *p, uint32_t len)
{
  uint32_t resp = 0;
  uint32_t number = 0;
  struct pb_reader_s r;
  uint32_t field;
  uint32_t sub_len;
  uint8_t wire;
  FAR const uint8_t *sub;

  pb_field_varint(p, len, 1, &resp);

  switch (msg_id)
    {
    case RPC_ID_RESP_WIFI_SCAN_START:
      priv->scan_start_status = (int)resp;
      priv->scan_start_resp = true;
      break;

    case RPC_ID_RESP_GET_DHCP_DNS:
      hosted_rpc_dhcp_dns_status(priv, p, len);
      break;

    case RPC_ID_RESP_GET_MAC:
      {
        FAR const uint8_t *mac;
        uint32_t mac_len = 0;

        if (pb_field_bytes(p, len, 1, &mac, &mac_len) && mac_len >= 6)
          {
            memcpy(priv->sta_mac, mac, 6);
          }

        priv->get_mac_resp = true;
      }
      break;

    case RPC_ID_RESP_WIFI_SCAN_GET_APNUM:
      pb_field_varint(p, len, 2, &number);
      priv->scan_apnum_number = number;
      priv->scan_apnum_resp = true;
      break;

    case RPC_ID_RESP_WIFI_SCAN_GET_APREC:
      pb_field_varint(p, len, 2, &number);
      priv->scan_ap_count = 0;

      /* ap_records = 3 is a repeated wifi_ap_record sub-message. */
      r.p = p;
      r.len = len;

      while (pb_read_tag(&r, &field, &wire))
        {
          if (field == 3 && wire == 2)
            {
              if (pb_read_varint(&r, &sub_len) && sub_len <= r.len)
                {
                  sub = r.p;
                  r.p += sub_len;
                  r.len -= sub_len;
                  hosted_rpc_parse_ap_record(priv, sub, sub_len);
                  continue;
                }
            }

          if (!pb_skip(&r, wire))
            {
              break;
            }
        }

      priv->scan_aprec_resp = true;
      break;

    default:
      break;
    }

  /* rpc resp traces flood the console during activate/TLS polling. */
}

static void hosted_rpc_dispatch(FAR struct metalio_hosted_s *priv,
                                FAR const uint8_t *data, uint16_t len)
{
  uint32_t msg_type = 0;
  uint32_t msg_id = 0;
  FAR const uint8_t *payload = NULL;
  uint32_t payload_len = 0;

  pb_field_varint(data, len, 1, &msg_type);
  pb_field_varint(data, len, 2, &msg_id);
  pb_field_bytes(data, len, msg_id, &payload, &payload_len);

  if (msg_type == RPC_MSG_TYPE_EVENT)
    {
      switch (msg_id)
        {
        case RPC_ID_EVENT_STA_CONNECTED:
          hosted_rpc_event_sta_connected(priv, payload, payload_len);
          return;

        case RPC_ID_EVENT_STA_DISCONNECTED:
          hosted_rpc_event_sta_disconnected(priv, payload, payload_len);
          return;

        case RPC_ID_EVENT_STA_SCAN_DONE:
          hosted_rpc_event_sta_scan_done(priv, payload, payload_len);
          return;

        case RPC_ID_EVENT_DHCP_DNS_STATUS:
          hosted_rpc_event_dhcp_dns(priv, payload, payload_len);
          return;

        default:
          /* Skip per-event spam (e.g. id=773) — floods serial during STA. */
          return;
        }
    }

  if (msg_type == RPC_MSG_TYPE_RESP)
    {
      hosted_rpc_resp(priv, msg_id, payload, payload_len);
      return;
    }

  /* Unknown RPC messages: keep quiet on the hot path. */
}

/* Unwrap the protocomm_pserial TLV carried on a serial-interface frame and
 * feed the embedded protobuf Rpc message to the dispatcher.
 */

static bool hosted_rpc_parse_tlv(FAR const uint8_t *p, uint16_t len,
                                 FAR const uint8_t **data,
                                 FAR uint16_t *data_len)
{
  uint16_t n;
  uint16_t pos = 0;

  /* [0x01][ep_len:2][epname...][0x02][data_len:2][data...] */

  if (len < 3 || p[pos] != RPC_TLV_TYPE_EPNAME)
    {
      return false;
    }

  pos++;
  n = (uint16_t)p[pos] | ((uint16_t)p[pos + 1] << 8);
  pos += 2;
  if (pos + n > len)
    {
      return false;
    }

  pos += n;

  if (pos + 3 > len || p[pos] != RPC_TLV_TYPE_DATA)
    {
      return false;
    }

  pos++;
  n = (uint16_t)p[pos] | ((uint16_t)p[pos + 1] << 8);
  pos += 2;
  if (pos + n > len)
    {
      return false;
    }

  *data = &p[pos];
  *data_len = n;
  return true;
}

static void hosted_rpc_handle_frame(FAR struct metalio_hosted_s *priv,
                                    FAR const uint8_t *payload,
                                    uint16_t payload_len)
{
  FAR const uint8_t *data;
  uint16_t data_len;

  if (hosted_rpc_parse_tlv(payload, payload_len, &data, &data_len))
    {
      hosted_rpc_dispatch(priv, data, data_len);
    }
}

/* Derive how many bytes the slave currently has available to read using the
 * official streaming-mode arithmetic.  `raw` is the PKT_LEN register value,
 * a cumulative counter that wraps at ESP_RX_BYTE_MAX; the host has already
 * consumed `rx_byte_count` bytes from the stream.
 */

static int hosted_rx_avail(FAR struct metalio_hosted_s *priv,
                           FAR uint32_t *avail,
                           FAR uint32_t *int_raw_out)
{
  uint32_t int_raw;
  uint32_t int_st;
  uint32_t raw;
  uint32_t len;
  int ret;

  /* Follow the official esp-hosted sdio_read_task() flow: read the
   * slave->host raw interrupt first, then PKT_LEN.  Only a NEW_PACKET
   * interrupt means the slave has committed a whole packet.  The previous
   * code polled PKT_LEN directly; a stale cumulative counter (or a
   * counter reset by the slave) then produced a bogus non-zero length and
   * a CMD53 read the slave could not satisfy, ending in DTO.
   */

  ret = hosted_reg_read32(priv, ESP_SDIO_REG_INT_RAW, &int_raw);
  if (ret < 0)
    {
      return ret;
    }

  if (int_raw_out != NULL)
    {
      *int_raw_out = int_raw;
    }

  ret = hosted_reg_read32(priv, ESP_SDIO_REG_INT_ST, &int_st);
  if (ret < 0)
    {
      int_st = 0;
    }

  (void)int_st;

  ret = hosted_reg_read32(priv, ESP_SDIO_REG_PKT_LEN, &raw);
  if (ret < 0)
    {
      return ret;
    }

  if (raw == 0xffffffffu)
    {
      /* All 32 bits set means the SDIO data lines are floating (bus fault). */
      return -EIO;
    }

  if ((int_raw & ESP_SDIO_INT_NEW_PACKET) == 0)
    {
      *avail = 0;
      return OK;
    }

  /* NOTE: do NOT clear NEW_PACKET here.  hosted_rx_avail() is called both
   * by the poll worker (to detect readiness) and by the receive path (to
   * drain).  Clearing here makes the second call see no NEW_PACKET and skip
   * the FIFO read.  The interrupt is cleared in hosted_drain_rx() only
   * after the FIFO has actually been drained.
   */

  raw &= ESP_SLAVE_LEN_MASK;

  if (raw >= priv->rx_byte_count)
    {
      len = (raw + ESP_RX_BYTE_MAX - priv->rx_byte_count) % ESP_RX_BYTE_MAX;
    }
  else
    {
      len = (ESP_RX_BYTE_MAX - priv->rx_byte_count) + raw;
    }

  *avail = len;
  return OK;
}

/* Append `len` bytes to the persistent RX buffer, growing it as needed. */

static int hosted_rx_append(FAR struct metalio_hosted_s *priv,
                            FAR const uint8_t *data, unsigned int len)
{
  unsigned int need = priv->rx_len + len;
  FAR uint8_t *nb;
  unsigned int ncap;

  if (need > priv->rx_cap)
    {
      ncap = priv->rx_cap ? priv->rx_cap : 512;
      while (ncap < need)
        {
          ncap <<= 1;
        }

      nb = kmm_realloc(priv->rx_buf, ncap);
      if (nb == NULL)
        {
          return -ENOMEM;
        }

      priv->rx_buf = nb;
      priv->rx_cap = ncap;
    }

  memcpy(priv->rx_buf + priv->rx_len, data, len);
  priv->rx_len = need;
  return OK;
}

/* Drain available bytes from the slave TX FIFO into the local RX buffer.
 *
 * The C5 slave runs in streaming mode (the MetalioClaw4 reference enables
 * CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE), where PKT_LEN is a
 * cumulative counter rather than a "bytes left" value.  The whole available
 * stream must be read in a single block-mode CMD53; the read address encodes
 * the un-padded length and the slave zero-fills the trailing padding.
 */

static int hosted_drain_rx(FAR struct metalio_hosted_s *priv)
{
  uint32_t avail;
  uint32_t int_raw = 0;
  FAR uint8_t *buf;
  int ret;

  ret = hosted_rx_avail(priv, &avail, &int_raw);
  if (ret < 0)
    {
      return ret;
    }

  if (avail == 0)
    {
      return OK;
    }

  /* Clear the WHOLE interrupt snapshot BEFORE the FIFO read, exactly like
   * the official sdio_read_task(): it does sdio_clear_intr(interrupts) with
   * the full INT_RAW value, not just NEW_PACKET.  Leaving RX_SOF/RX_EOF/
   * RX_START/EXT_BIT3 set keeps the C5 slave state machine from re-asserting
   * NEW_PACKET for subsequent packets, which is why RX stalled right after
   * the first ESPInit event.  Clearing before the read also preserves any
   * NEW_PACKET the slave asserts while the CMD53 is in flight.
   */

  ret = hosted_clear_slave_intr(priv, int_raw);
  if (ret < 0)
    {
      syslog(LOG_ERR, "esp-hosted: clear intr ret=%d mask=0x%08lx\n",
             ret, (unsigned long)int_raw);
    }

  buf = kmm_malloc(avail);
  if (buf == NULL)
    {
      return -ENOMEM;
    }

  ret = hosted_fifo_read(priv, buf, avail);
  if (ret < 0)
    {
      kmm_free(buf);
      return ret;
    }

  priv->rx_byte_count = (priv->rx_byte_count + avail) % ESP_RX_BYTE_MAX;

  ret = hosted_rx_append(priv, buf, avail);
  kmm_free(buf);
  return ret;
}

/* Extract one complete frame from the local RX buffer, if present.
 *
 * STA-interface frames are delivered as netpkt_t; serial/priv control frames
 * (RPC responses/events) are consumed here but handed off to the RPC event
 * dispatcher only once that decoder lands.  The esp_payload_header carries no
 * magic byte, so `offset == 12` plus a valid `if_type` acts as the sync check.
 */

static FAR netpkt_t *hosted_parse_frame(FAR struct metalio_hosted_s *priv)
{
  FAR netpkt_t *pkt;
  uint16_t len;
  uint16_t offset;
  uint8_t if_type;
  int ret;

  if (priv->rx_len < ESP_HOSTED_PAYLOAD_HDR_LEN)
    {
      return NULL;
    }

  offset = (uint16_t)priv->rx_buf[4] | ((uint16_t)priv->rx_buf[5] << 8);
  if_type = priv->rx_buf[0] & 0x0f;

  if (offset != ESP_HOSTED_PAYLOAD_HDR_LEN || if_type == 0 ||
      if_type >= ESP_HOSTED_IF_MAX)
    {
      /* Lost sync: drop one byte and resync on the next poll. */

      memmove(priv->rx_buf, priv->rx_buf + 1, --priv->rx_len);
      return NULL;
    }

  len = (uint16_t)priv->rx_buf[2] | ((uint16_t)priv->rx_buf[3] << 8);
  if (len > ESP_HOSTED_MAX_PAYLOAD)
    {
      /* Corrupt length: drop the header. */

      memmove(priv->rx_buf, priv->rx_buf + ESP_HOSTED_PAYLOAD_HDR_LEN,
              priv->rx_len - ESP_HOSTED_PAYLOAD_HDR_LEN);
      priv->rx_len -= ESP_HOSTED_PAYLOAD_HDR_LEN;
      return NULL;
    }

  if (priv->rx_len < ESP_HOSTED_PAYLOAD_HDR_LEN + len)
    {
      return NULL;
    }

  if (if_type == ESP_HOSTED_IF_STA)
    {
      pkt = netpkt_alloc(&priv->dev, NETPKT_RX);
      if (pkt == NULL)
        {
          return NULL;
        }

      ret = netpkt_copyin(&priv->dev, pkt,
                          priv->rx_buf + ESP_HOSTED_PAYLOAD_HDR_LEN, len, 0);
      if (ret < 0)
        {
          netpkt_free(&priv->dev, pkt, NETPKT_RX);
          return NULL;
        }

      priv->rx_pkts++;
    }
  else
    {
      pkt = NULL;

      /* Serial-interface frames carry the protocomm_pserial RPC control
       * path (responses and events).  Decode them in-place.
       */

      if (if_type == ESP_HOSTED_IF_SERIAL)
        {
          hosted_rpc_handle_frame(priv,
                                  priv->rx_buf + ESP_HOSTED_PAYLOAD_HDR_LEN,
                                  len);
        }
    }

  memmove(priv->rx_buf, priv->rx_buf + ESP_HOSTED_PAYLOAD_HDR_LEN + len,
          priv->rx_len - (ESP_HOSTED_PAYLOAD_HDR_LEN + len));
  priv->rx_len -= ESP_HOSTED_PAYLOAD_HDR_LEN + len;

  return pkt;
}

/* Pulse the C5 RESET pin (active-high EN on this board) and wait for the
 * slave ROM + esp-hosted app to re-enumerate on SDIO.  Used both for a
 * mid-boot second chance and for late recovery when the first probe window
 * missed a slow C5 bring-up.
 */

static void hosted_c5_hw_reset(void)
{
  bool run = (BOARD_HOSTED_SDIO_RESET_ACTIVE_HIGH != 0);

  esp_configgpio(BOARD_HOSTED_SDIO_RESET_GPIO, OUTPUT);
  esp_gpiowrite(BOARD_HOSTED_SDIO_RESET_GPIO, !run); /* hold in reset */
  up_udelay(50000);
  esp_gpiowrite(BOARD_HOSTED_SDIO_RESET_GPIO, run);  /* release / run */
  up_udelay(5000 * 1000);
}

/* Initialise the C5 SDIO card (control plane, all polled CMD52/CMD5/CMD3/
 * CMD7).  Returns OK once function 1 is enabled and the block size is set.
 * When quiet is true, skip the per-attempt warnings (retry loops would
 * otherwise flood the console).
 */

/* Re-apply Slot1 pinmux + soft-reset the shared SDMMC host.  Call after
 * Slot0 empty-slot recovery and after C5 HW reset — not before every probe
 * attempt, or the host clock drops and the C5 SDIO slave never stays
 * enumerated.
 */

void esp32p4_sdmmc_slot1_prepare(FAR struct sdio_dev_s *dev);

static int hosted_sdio_init(FAR struct metalio_hosted_s *priv, bool quiet)
{
  FAR struct sdio_dev_s *dev = priv->sdio;
  int ret;

  /* 1. I/O reset (CCCR 0x06, RES bit).  The C5 was already hardware-reset
   *    via its RESET line in sdio_initialize(1); this is the standard SDIO
   *    software reset and is best-effort.
   */

  ret = sdio_io_rw_direct(dev, true, 0, SDIO_CCCR_IOABORT, 0x08, NULL);
  if (ret < 0 && !quiet)
    {
      syslog(LOG_WARNING, "esp-hosted: SDIO I/O reset failed: %d\n", ret);
    }

  /* 2. Standard SDIO probe: CMD0, CMD5 (twice), CMD3, CMD7, 4-bit bus. */

  ret = sdio_probe(dev);
  if (ret < 0)
    {
      if (!quiet)
        {
          syslog(LOG_WARNING,
                 "esp-hosted: sdio_probe failed (ret=%d) — C5 firmware not "
                 "running or pins unconnected\n",
                 ret);
        }

      return ret;
    }

  /* 3. Enable I/O function 1 (the ESP-Hosted data/register function). */

  ret = sdio_enable_function(dev, ESP_SDIO_FUNC);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "esp-hosted: enable func1 failed: %d\n", ret);
      return ret;
    }

  /* 4. Function 0 and function 1 block size (512 bytes). */

  ret = sdio_set_blocksize(dev, 0, ESP_SDIO_BLOCKLEN);
  if (ret < 0)
    {
      return ret;
    }

  ret = sdio_set_blocksize(dev, ESP_SDIO_FUNC, ESP_SDIO_BLOCKLEN);
  if (ret < 0)
    {
      return ret;
    }

  /* Bump the Slot1 clock to the transfer rate (40 MHz) now that the card
   * has enumerated.  Slot0 and Slot1 share the SDMMC host clock divider, so
   * this mirrors ESP-IDF's two-slot behaviour of reprogramming the shared
   * divider when a slot becomes active.
   */

  SDIO_CLOCK(dev, CLOCK_MMC_TRANSFER);

  syslog(LOG_INFO, "esp-hosted: C5 SDIO init done, func1 enabled\n");
  return OK;
}

/* Ask the C5 slave to open its host->slave data path by writing the
 * ESP_OPEN_DATA_PATH interrupt bit (bit 0) to HOST_TO_SLAVE_INTR, which is
 * ESP_SLAVE_SCRATCH_REG_7 (SDIO function-1 address 0x8C).  Until the slave
 * receives this, its coprocessor recv_task keeps datapath == 0 and never
 * consumes FIFO data written by the host, so RPC requests never get a reply.
 */

static int hosted_sdio_open_data_path(FAR struct metalio_hosted_s *priv)
{
  uint8_t val = ESP_SDIO_INT_OPEN_DATA_PATH;

  return sdio_io_rw_direct(priv->sdio, true, ESP_SDIO_FUNC,
                           ESP_SDIO_REG_SLAVE_INT, val, NULL);
}

/* Re-probe the C5 if the boot-time window left link_up=false.  Without this,
 * metalio_esp_hosted_initialize() returns OK on re-entry (g_hosted already
 * set) and every scan/connect permanently fails with ENETDOWN.
 */

static int hosted_ensure_link(FAR struct metalio_hosted_s *priv)
{
  int ret;
  int i;

  if (priv == NULL || priv->sdio == NULL)
    {
      return -ENODEV;
    }

  if (priv->link_up)
    {
      return OK;
    }

  syslog(LOG_WARNING,
         "esp-hosted: SDIO link down, resetting C5 and re-probing\n");

  hosted_c5_hw_reset();
  priv->wifi_init = false;
  priv->mac_queried = false;
  priv->sta_connected = false;
  esp32p4_sdmmc_slot1_prepare(priv->sdio);
  SDIO_CLOCK(priv->sdio, CLOCK_IDMODE);

  for (i = 0; i < 80; i++)
    {
      ret = hosted_sdio_init(priv, (i % 10) != 0);
      if (ret == OK)
        {
          priv->link_up = true;
          ret = hosted_sdio_open_data_path(priv);
          syslog(LOG_INFO,
                 "esp-hosted: link recovered after %d tries (open-data=%d)\n",
                 i + 1, ret);
          return OK;
        }

      usleep(100 * 1000);
    }

  syslog(LOG_ERR, "esp-hosted: link recovery failed after C5 reset\n");
  return -ENETDOWN;
}

/* Periodic RX poll.  Slot1 has no SDMMC interrupt, so drive the upper-half
 * receive path from the HP work queue while the interface is up.
 */

static void hosted_poll_worker(void *arg)
{
  FAR struct metalio_hosted_s *priv = arg;
  uint32_t avail = 0;
  bool ready = false;

  /* Poll for slave->host data as soon as the SDIO link is up, regardless of
   * whether the netdev has been ifup'd.  The RPC control path (scan results,
   * connect/disconnect/DHCP events) travels over the same SDIO FIFO as the
   * STA data plane, and it must be drained and dispatched even while the
   * interface is administratively down.  netdev_lower_rxready() only drops
   * the actual STA frames when the interface is down; the control frames are
   * consumed inside hosted_receive().
   */

  nxmutex_lock(&priv->lock);
  if (priv->link_up && hosted_rx_avail(priv, &avail, NULL) == OK && avail > 0)
    {
      ready = true;
    }

  nxmutex_unlock(&priv->lock);

  if (ready)
    {
      netdev_lower_rxready(&priv->dev);
    }

  work_queue(LPWORK, &priv->pollwork, hosted_poll_worker, priv,
             MSEC2TICK(10));
}

/****************************************************************************
 * Netdev lower-half operations
 ****************************************************************************/

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
  FAR struct metalio_hosted_s *priv = (FAR struct metalio_hosted_s *)dev;
  uint8_t *buf;
  unsigned int len;
  unsigned int txlen;
  int ret;

  if (!priv->link_up)
    {
      netpkt_free(dev, pkt, NETPKT_TX);
      return -ENETDOWN;
    }

  len = netpkt_getdatalen(dev, pkt);
  if (len > ESP_HOSTED_MAX_PAYLOAD)
    {
      netpkt_free(dev, pkt, NETPKT_TX);
      return -E2BIG;
    }

  /* The slave re-frames 802.3 payloads via esp_wifi_internal_tx(), which
   * drops sub-60-byte frames (e.g. 42-byte ARP requests) instead of
   * zero-padding them like a hardware MAC would.  Pad to the Ethernet
   * minimum so broadcast ARP actually reaches the AP.
   */

  txlen = len < 60 ? 60 : len;

  buf = kmm_malloc(ESP_HOSTED_PAYLOAD_HDR_LEN + txlen);
  if (buf == NULL)
    {
      netpkt_free(dev, pkt, NETPKT_TX);
      return -ENOMEM;
    }

  ret = netpkt_copyout(dev, buf + ESP_HOSTED_PAYLOAD_HDR_LEN, pkt, len, 0);
  if (ret < 0)
    {
      kmm_free(buf);
      netpkt_free(dev, pkt, NETPKT_TX);
      return ret;
    }

  if (txlen > len)
    {
      memset(buf + ESP_HOSTED_PAYLOAD_HDR_LEN + len, 0, txlen - len);
    }

  hosted_frame_put_hdr(buf, ESP_HOSTED_IF_STA, (uint16_t)txlen,
                       priv->tx_seq++);

  nxmutex_lock(&priv->lock);
  ret = hosted_fifo_write(priv, buf, ESP_HOSTED_PAYLOAD_HDR_LEN + txlen);
  nxmutex_unlock(&priv->lock);

  priv->tx_pkts++;

  kmm_free(buf);
  netpkt_free(dev, pkt, NETPKT_TX);
  return ret;
}

static FAR netpkt_t *hosted_receive(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct metalio_hosted_s *priv = (FAR struct metalio_hosted_s *)dev;
  FAR netpkt_t *pkt = NULL;

  if (!priv->link_up)
    {
      return NULL;
    }

  nxmutex_lock(&priv->lock);
  if (hosted_drain_rx(priv) == OK)
    {
      /* Drain reads every byte the slave has committed, but the stream may
       * hold several frames (e.g. a response plus queued events).  Parse
       * them all: RPC control frames are dispatched in-place, and the first
       * STA data frame is returned to the caller.  Stop only when a frame
       * is incomplete (parse made no progress) or a STA packet was found.
       */

      while (priv->rx_len >= ESP_HOSTED_PAYLOAD_HDR_LEN)
        {
          unsigned int before = priv->rx_len;
          FAR netpkt_t *fp = hosted_parse_frame(priv);

          if (fp != NULL)
            {
              pkt = fp;
              break;
            }

          if (priv->rx_len == before)
            {
              /* Incomplete frame: need more bytes from the slave. */
              break;
            }

          /* hosted_parse_frame() consumed one RPC frame (or dropped a byte
           * during resync); keep going.
           */
        }
    }

  nxmutex_unlock(&priv->lock);
  return pkt;
}

static const struct netdev_ops_s g_hosted_ops =
{
  .ifup = hosted_ifup,
  .ifdown = hosted_ifdown,
  .transmit = hosted_transmit,
  .receive = hosted_receive,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int metalio_esp_hosted_initialize(void)
{
  FAR struct metalio_hosted_s *priv;
  int ret;
  int i;

  if (g_hosted != NULL)
    {
      /* Boot may have registered eth0 with link_up=false after a short
       * probe window.  Retry (with C5 HW reset) so later scan/connect work.
       */
      return hosted_ensure_link(g_hosted);
    }

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->dev.ops = &g_hosted_ops;
  nxmutex_init(&priv->lock);
  nxmutex_init(&priv->scan_lock);

  /* The upper-half netdev gates TX on a non-zero TX quota and RX packet
   * allocation on the RX quota (netdev_upper_can_tx() / netpkt_alloc()).
   * priv is kmm_zalloc'd so both quotas default to zero, which makes the
   * upper half silently refuse to transmit or receive a single frame — the
   * DHCP/ARP/Ethernet data plane never even reaches hosted_transmit().
   */
  priv->dev.quota[NETPKT_TX] = 1;
  priv->dev.quota[NETPKT_RX] = CONFIG_IOB_NBUFFERS / 2;

  /* Route the upper-half RX work to LPWORK (priority 100) instead of the
   * default HPWORK (224).  hosted_receive() drives the SDMMC CMD53 data
   * phase with a polled busy-loop (Slot1 has no interrupt); running that
   * busy-loop at HPWORK priority starves the ~100/105-priority network
   * threads that are blocked in a 20 ms ARP wait, so their watchdog timeout
   * never gets scheduled and DNS/ARP wedges.  LPWORK is below those threads,
   * so they can preempt the SDMMC poll and make progress.
   */
  priv->dev.priority = LPWORK;

  /* Assign a stable, locally-administered MAC.  The C5 runs the 802.11
   * MAC and forwards the host's Ethernet frames, so the host-side netdev
   * needs a valid station address for ARP/DHCP before the interface is up.
   */

  priv->dev.netdev.d_mac.ether.ether_addr_octet[0] = 0x02;
  priv->dev.netdev.d_mac.ether.ether_addr_octet[1] = 0x11;
  priv->dev.netdev.d_mac.ether.ether_addr_octet[2] = 0x22;
  priv->dev.netdev.d_mac.ether.ether_addr_octet[3] = 0x33;
  priv->dev.netdev.d_mac.ether.ether_addr_octet[4] = 0x44;
  priv->dev.netdev.d_mac.ether.ether_addr_octet[5] = 0x55;

  /* Initialise the SDMMC Slot1 host.  This also asserts/releases the C5
   * RESET line, routes the Slot1 signals through the GPIO matrix and enables
   * the Slot1 card clock.
   */

  priv->sdio = sdio_initialize(1);
  if (priv->sdio == NULL)
    {
      syslog(LOG_ERR, "esp-hosted: sdio_initialize(1) failed\n");
      kmm_free(priv);
      return -ENODEV;
    }

  /* Enable the Slot1 card clock at identification frequency.  The Slot0
   * (SD card) path does this through the MMCSD layer (SDIO_CLOCK); the
   * direct SDIO probe used here must do it explicitly, otherwise CMD5 is
   * emitted with the C5 clock disabled and always times out.
   */

  SDIO_CLOCK(priv->sdio, CLOCK_IDMODE);

  /* Probe + enable function 1.  The C5's SDIO function only becomes
   * responsive after its RESET line is released while the ROM and the
   * esp-hosted slave app boot.  After a large host flash / USB reset the
   * C5 can take several seconds, so retry ~8 s and do a second HW reset
   * halfway.  If it never comes up, still register eth0; scan/connect will
   * call hosted_ensure_link() for a late recovery.
   */

  ret = -ETIMEDOUT;
  for (i = 0; i < 80; i++)
    {
      if (i == 40)
        {
          syslog(LOG_WARNING,
                 "esp-hosted: C5 still silent, second HW reset\n");
          hosted_c5_hw_reset();
          esp32p4_sdmmc_slot1_prepare(priv->sdio);
          SDIO_CLOCK(priv->sdio, CLOCK_IDMODE);
        }

      ret = hosted_sdio_init(priv, (i % 10) != 0);
      if (ret == OK)
        {
          break;
        }

      usleep(100 * 1000);
    }

  if (ret < 0)
    {
      priv->link_up = false;
    }
  else
    {
      priv->link_up = true;

      ret = hosted_sdio_open_data_path(priv);
      syslog(LOG_INFO, "esp-hosted: open-data-path handshake ret=%d\n", ret);

      {
        uint32_t ena_fn1 = 0;
        uint32_t ena_slc0 = 0;
        uint32_t st = 0;

        hosted_reg_read32(priv, ESP_SDIO_REG_INT_ENA, &ena_fn1);
        hosted_reg_read32(priv, 0x0ecu, &ena_slc0);
        hosted_reg_read32(priv, ESP_SDIO_REG_INT_ST, &st);
        syslog(LOG_INFO,
               "esp-hosted: intr ena fn1=0x%08lx slc0=0x%08lx st=0x%08lx "
               "(new_packet_en=%u)\n",
               (unsigned long)ena_fn1, (unsigned long)ena_slc0,
               (unsigned long)st,
               (unsigned int)((ena_fn1 >> 23) & 1u));
      }
    }

  /* Register as Ethernet for now; switch to NET_LL_IEEE80211 once
   * CONFIG_DRIVERS_IEEE80211 and the full esp-hosted SDIO transport are
   * enabled.
   */

  ret = netdev_lower_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      syslog(LOG_ERR, "esp-hosted: netdev register failed: %d\n", ret);
      kmm_free(priv);
      return ret;
    }

  g_hosted = priv;

  /* Start the RX poll worker (Slot1 has no interrupt source). */

  work_queue(LPWORK, &priv->pollwork, hosted_poll_worker, priv, 0);

  syslog(LOG_INFO,
         "esp-hosted: eth0 registered (SDIO CMD=%d CLK=%d D0=%d @%dkHz, "
         "link=%s)\n",
         BOARD_HOSTED_SDIO_CMD_GPIO, BOARD_HOSTED_SDIO_CLK_GPIO,
         BOARD_HOSTED_SDIO_D0_GPIO, BOARD_HOSTED_SDIO_CLOCK_KHZ,
         priv->link_up ? "up" : "down (C5 firmware pending)");
  return OK;
}

static int hosted_ensure_wifi_started(FAR struct metalio_hosted_s *priv)
{
  uint8_t rpc[PB_BUF_CAP];
  uint16_t rpc_len;
  int ret;

  if (priv->wifi_init)
    {
      return OK;
    }

  rpc_len = hosted_rpc_wifi_init(rpc, sizeof(rpc));
  if (rpc_len == 0)
    {
      return -E2BIG;
    }

  ret = hosted_rpc_send(priv, rpc, rpc_len);
  if (ret < 0)
    {
      syslog(LOG_INFO, "esp-hosted: wifi_init send failed: %d\n", ret);
      return ret;
    }

  rpc_len = hosted_rpc_wifi_set_mode(rpc, sizeof(rpc));
  if (rpc_len == 0)
    {
      return -E2BIG;
    }

  ret = hosted_rpc_send(priv, rpc, rpc_len);
  if (ret < 0)
    {
      syslog(LOG_INFO, "esp-hosted: wifi_set_mode send failed: %d\n", ret);
      return ret;
    }

  rpc_len = hosted_rpc_wifi_start(rpc, sizeof(rpc));
  if (rpc_len == 0)
    {
      return -E2BIG;
    }

  ret = hosted_rpc_send(priv, rpc, rpc_len);
  if (ret < 0)
    {
      syslog(LOG_INFO, "esp-hosted: wifi_start send failed: %d\n", ret);
      return ret;
    }

  priv->wifi_init = true;
  syslog(LOG_INFO, "esp-hosted: wifi init+setmode+start sent OK\n");

  /* Fetch the slave's real STA MAC and use it for the host netdev.  The
   * C5's 802.11 stack always transmits using its own eFuse station MAC
   * (esp_wifi_internal_tx() re-frames our 802.3 payload), so DHCP/ARP
   * replies come back addressed to that MAC.  If the host netdev keeps an
   * unrelated locally-administered MAC, the NuttX DHCP client drops every
   * OFFER because the echoed chaddr does not match.
   */

  if (!priv->mac_queried)
    {
      int timeout;

      priv->get_mac_resp = false;
      rpc_len = hosted_rpc_wifi_get_mac(rpc, sizeof(rpc));
      if (rpc_len == 0)
        {
          return -E2BIG;
        }

      ret = hosted_rpc_send(priv, rpc, rpc_len);
      if (ret < 0)
        {
          syslog(LOG_INFO, "esp-hosted: wifi_get_mac send failed: %d\n", ret);
          return ret;
        }

      for (timeout = 0; timeout < 2000 && !priv->get_mac_resp; timeout += 10)
        {
          usleep(10 * 1000);
        }

      if (!priv->get_mac_resp)
        {
          syslog(LOG_WARNING,
                 "esp-hosted: no Resp_WifiGetMac (timeout), keeping "
                 "default MAC\n");
        }
      else
        {
          memcpy(priv->dev.netdev.d_mac.ether.ether_addr_octet,
                 priv->sta_mac, 6);
          priv->mac_queried = true;
          syslog(LOG_INFO,
                 "esp-hosted: STA MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                 priv->sta_mac[0], priv->sta_mac[1], priv->sta_mac[2],
                 priv->sta_mac[3], priv->sta_mac[4], priv->sta_mac[5]);
        }
    }

  return OK;
}

int metalio_esp_hosted_start_wifi(void)
{
  FAR struct metalio_hosted_s *priv = g_hosted;
  int ret;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  ret = hosted_ensure_link(priv);
  if (ret < 0)
    {
      return ret;
    }

  return hosted_ensure_wifi_started(priv);
}

int metalio_esp_hosted_connect(FAR const char *ssid, FAR const char *pass)
{
  FAR struct metalio_hosted_s *priv = g_hosted;
  uint8_t rpc[PB_BUF_CAP];
  uint16_t rpc_len;
  int ret;

  if (priv == NULL || ssid == NULL)
    {
      return -ENODEV;
    }

  strlcpy(priv->ssid, ssid, sizeof(priv->ssid));
  priv->pass[0] = '\0';
  if (pass)
    {
      strlcpy(priv->pass, pass, sizeof(priv->pass));
    }

  ret = hosted_ensure_link(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* Official esp_wifi_remote flow (one-time init, then per-connect config):
   *   Req_WifiInit(278) -> Req_WifiSetMode(260, STA) -> Req_WifiStart(280)
   *   -> Req_WifiSetConfig(284) -> Req_WifiConnect(282).
   */

  ret = hosted_ensure_wifi_started(priv);
  if (ret < 0)
    {
      return ret;
    }

  rpc_len = hosted_rpc_wifi_set_config(rpc, sizeof(rpc), ssid, pass);
  if (rpc_len == 0)
    {
      return -E2BIG;
    }

  ret = hosted_rpc_send(priv, rpc, rpc_len);
  if (ret < 0)
    {
      return ret;
    }

  rpc_len = hosted_rpc_wifi_connect(rpc, sizeof(rpc));
  if (rpc_len == 0)
    {
      return -E2BIG;
    }

  ret = hosted_rpc_send(priv, rpc, rpc_len);

  syslog(LOG_INFO, "esp-hosted: init+set-config+connect SSID=%s (ret=%d)\n",
         ssid, ret);
  return ret;
}

int metalio_esp_hosted_disconnect(void)
{
  FAR struct metalio_hosted_s *priv = g_hosted;
  uint8_t rpc[PB_BUF_CAP];
  uint16_t rpc_len;
  int ret;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  if (!priv->link_up)
    {
      priv->sta_connected = false;
      return -ENETDOWN;
    }

  rpc_len = hosted_rpc_wifi_disconnect(rpc, sizeof(rpc));
  if (rpc_len == 0)
    {
      return -E2BIG;
    }

  ret = hosted_rpc_send(priv, rpc, rpc_len);
  priv->sta_connected = false;
  syslog(LOG_INFO, "esp-hosted: WifiDisconnect ret=%d\n", ret);
  return ret;
}

int metalio_esp_hosted_scan(FAR struct metalio_wifi_ap_s *out, int max_ap)
{
  FAR struct metalio_hosted_s *priv = g_hosted;
  uint8_t rpc[PB_BUF_CAP];
  uint16_t rpc_len;
  uint32_t number;
  int ret;
  int i;
  int n;
  int timeout;

  syslog(LOG_INFO,
         "esp-hosted: scan: enter priv=%p link_up=%d wifi_init=%d max_ap=%d\n",
         (FAR void *)priv, priv != NULL ? (int)priv->link_up : -1,
         priv != NULL ? (int)priv->wifi_init : -1, max_ap);

  if (priv == NULL || out == NULL || max_ap <= 0)
    {
      syslog(LOG_ERR, "esp-hosted: scan: bad args (priv/out/max_ap)\n");
      return -EINVAL;
    }

  ret = hosted_ensure_link(priv);
  if (ret < 0)
    {
      syslog(LOG_ERR, "esp-hosted: scan: link recovery failed (%d)\n", ret);
      return ret;
    }

  /* The scan uses shared per-adapter state (scan_* fields above).  Two
   * concurrent scans (e.g. the boot-time self-test and a UI scan_task) would
   * both reset scan_apnum_resp/scan_aprec_resp and match the wrong RPC
   * responses, leaving one or both callers to time out.  Serialise them.
   */
  nxmutex_lock(&priv->scan_lock);

  ret = hosted_ensure_wifi_started(priv);
  if (ret < 0)
    {
      goto done;
    }

  /* Boot auto-connect often leaves the C5 mid-association (or retrying an
   * unreachable SSID).  MetalioClaw4 uses esp_wifi_scan_start(..., false)
   * (non-blocking) and waits for WIFI_EVENT_SCAN_DONE.  Blocking
   * Req_WifiScanStart on the slave holds the RPC reply for ~9s and frequently
   * times out / returns ESP_ERR_WIFI_STATE (0x3006) while STA is busy.
   *
   * Always free the radio first, then start a non-blocking scan and wait for
   * Event_StaScanDone before fetching AP records.
   */
  rpc_len = hosted_rpc_wifi_disconnect(rpc, sizeof(rpc));
  if (rpc_len > 0)
    {
      (void)hosted_rpc_send(priv, rpc, rpc_len);
    }

  priv->sta_connected = false;
  usleep(300 * 1000);

  for (i = 0; i < 3; i++)
    {
      priv->scan_start_resp = false;
      priv->scan_done_event = false;
      priv->scan_apnum_resp = false;
      priv->scan_aprec_resp = false;
      priv->scan_start_status = 0;
      priv->scan_done_number = 0;
      priv->scan_apnum_number = 0;
      priv->scan_ap_count = 0;

      /* block=false: Resp_WifiScanStart returns as soon as the scan is
       * armed; completion is signalled by Event_StaScanDone (msg 774).
       */
      rpc_len = hosted_rpc_wifi_scan_start(rpc, sizeof(rpc), false);
      if (rpc_len == 0)
        {
          ret = -E2BIG;
          goto done;
        }

      ret = hosted_rpc_send(priv, rpc, rpc_len);
      if (ret < 0)
        {
          goto done;
        }

      for (timeout = 0; timeout < 5000; timeout += 50)
        {
          if (priv->scan_start_resp)
            {
              break;
            }

          usleep(50 * 1000);
        }

      if (!priv->scan_start_resp)
        {
          syslog(LOG_INFO,
                 "esp-hosted: scan: no Resp_WifiScanStart (timeout)\n");
          ret = -ETIMEDOUT;
          goto done;
        }

      if (priv->scan_start_status == 0)
        {
          break;
        }

      syslog(LOG_INFO, "esp-hosted: scan: WifiScanStart failed (err=0x%x)%s\n",
             priv->scan_start_status,
             (priv->scan_start_status == 0x3006 && i < 2) ?
             ", disconnect then retry" : "");

      if (priv->scan_start_status != 0x3006 || i >= 2)
        {
          ret = -EIO;
          goto done;
        }

      rpc_len = hosted_rpc_wifi_disconnect(rpc, sizeof(rpc));
      if (rpc_len > 0)
        {
          hosted_rpc_send(priv, rpc, rpc_len);
        }

      priv->sta_connected = false;
      usleep(500 * 1000);
    }

  if (priv->scan_start_status != 0)
    {
      ret = -EIO;
      goto done;
    }

  /* Wait for Event_StaScanDone.  Full 2.4/5 GHz all-channel scan on C5 is
   * typically 3–12 s; allow headroom for busy channels.
   */
  for (timeout = 0; timeout < 20000; timeout += 50)
    {
      if (priv->scan_done_event)
        {
          break;
        }

      usleep(50 * 1000);
    }

  if (!priv->scan_done_event)
    {
      syslog(LOG_INFO,
             "esp-hosted: scan: no Event_StaScanDone (timeout)\n");
      ret = -ETIMEDOUT;
      goto done;
    }

  syslog(LOG_INFO, "esp-hosted: scan: SCAN_DONE number=%lu\n",
         (unsigned long)priv->scan_done_number);

  rpc_len = hosted_rpc_wifi_scan_get_apnum(rpc, sizeof(rpc));
  if (rpc_len == 0)
    {
      ret = -E2BIG;
      goto done;
    }

  ret = hosted_rpc_send(priv, rpc, rpc_len);
  if (ret < 0)
    {
      goto done;
    }

  for (timeout = 0; timeout < 5000; timeout += 50)
    {
      if (priv->scan_apnum_resp)
        {
          break;
        }

      usleep(50 * 1000);
    }

  if (!priv->scan_apnum_resp)
    {
      syslog(LOG_INFO, "esp-hosted: scan: no Resp_WifiScanGetApNum (timeout)\n");
      ret = -ETIMEDOUT;
      goto done;
    }

  number = priv->scan_apnum_number;
  if (number == 0)
    {
      /* Prefer the event's count if GetApNum raced empty. */
      number = priv->scan_done_number;
    }

  if (number == 0)
    {
      ret = 0;
      goto done;
    }

  /* One SDIO serial frame is capped at ESP_HOSTED_MAX_PAYLOAD (1524).  A
   * full wifi_ap_record protobuf is ~60–100 bytes; asking for 27+ APs makes
   * Resp_WifiScanGetApRecords exceed the frame limit, so the host drops the
   * "corrupt" length and the UI times out with "扫描失败".  Cap the fetch.
   */
#define HOSTED_SCAN_AP_FETCH_MAX  12u
  if (number > HOSTED_SCAN_AP_FETCH_MAX)
    {
      syslog(LOG_INFO,
             "esp-hosted: scan: capping GetApRecords %lu -> %u (SDIO frame)\n",
             (unsigned long)number, HOSTED_SCAN_AP_FETCH_MAX);
      number = HOSTED_SCAN_AP_FETCH_MAX;
    }

  if (number > METALIO_WIFI_MAX_AP)
    {
      number = METALIO_WIFI_MAX_AP;
    }

  rpc_len = hosted_rpc_wifi_scan_get_ap_records(rpc, sizeof(rpc), number);
  if (rpc_len == 0)
    {
      ret = -E2BIG;
      goto done;
    }

  ret = hosted_rpc_send(priv, rpc, rpc_len);
  if (ret < 0)
    {
      goto done;
    }

  for (timeout = 0; timeout < 10000; timeout += 50)
    {
      if (priv->scan_aprec_resp)
        {
          break;
        }

      usleep(50 * 1000);
    }

  if (!priv->scan_aprec_resp)
    {
      syslog(LOG_INFO, "esp-hosted: scan: no Resp_WifiScanGetApRecords (timeout)\n");
      ret = -ETIMEDOUT;
      goto done;
    }

  n = (int)priv->scan_ap_count;
  if (n > max_ap)
    {
      n = max_ap;
    }

  for (i = 0; i < n; i++)
    {
      out[i] = priv->scan_aps[i];
    }

  ret = n;

done:
  syslog(LOG_INFO, "esp-hosted: scan: done ret=%d n_ap=%u\n",
         ret, (unsigned int)priv->scan_ap_count);
  nxmutex_unlock(&priv->scan_lock);
  return ret;
}

bool metalio_esp_hosted_is_connected(void)
{
  FAR struct metalio_hosted_s *priv = g_hosted;

  return priv != NULL && priv->sta_connected;
}

FAR const char *metalio_esp_hosted_get_ssid(void)
{
  FAR struct metalio_hosted_s *priv = g_hosted;

  if (priv == NULL)
    {
      return "";
    }

  if (priv->conn_ssid[0] != '\0')
    {
      return priv->conn_ssid;
    }

  return priv->ssid;
}
