/****************************************************************************
 * NT26 4G UART-Ethernet adapter for Metalio Claw4.
 *
 * The NT26 transport follows MetalioClaw4's four-byte SPIC framing and
 * UART Ethernet handshake.  UART1 is exposed as /dev/ttyS2 on this board;
 * UART2 remains /dev/ttyS1 for the existing Bluetooth audio client.
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <metalio/metalio.h>

#ifdef CONFIG_METALIO_NT26_4G

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <termios.h>
#include <arpa/inet.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/sched.h>
#include <nuttx/clock.h>
#include <nuttx/arch.h>
#include <arch/board/board.h>
#include <metalio/metalio.h>
#include "espressif/esp_gpio.h"
extern void esp_rom_delay_us(uint32_t us);

#define NT26_UART_PATH       "/dev/ttyS2"
#define NT26_BAUD            B2000000
#define NT26_MAX_FRAME       1604
#define NT26_MAX_PAYLOAD     1600
#define NT26_RX_STREAM_SIZE  4096
#define NT26_FRAME_ETH       0
#define NT26_FRAME_AT        1
#define NT26_TX_QUEUE_DEPTH  4

struct nt26_tx_packet_s
{
  FAR struct nt26_tx_packet_s *flink;
  size_t len;
  uint8_t priority;
  uint8_t data[];
};

struct nt26_rx_packet_s
{
  FAR struct nt26_rx_packet_s *flink;
  FAR netpkt_t *pkt;
};

struct nt26_state_s
{
  struct netdev_lowerhalf_s dev; /* Must be first */
  mutex_t tx_lock;
  mutex_t at_lock;
  mutex_t response_lock;
  mutex_t rx_lock;
  mutex_t tx_queue_lock;
  sem_t tx_ready;
  sem_t tx_ack;
  sem_t at_done;
  sem_t handshake_done;
  int uart_fd;
  pid_t rx_task;
  pid_t signal_task;
  pid_t tx_task;
  bool initialized;
  bool control_ready;
  bool carrier;
  bool waiting_at;
  bool handshake_seen;
  volatile bool tx_active;
  volatile unsigned long rx_bytes;
  volatile unsigned long rx_frames;
  volatile unsigned long rx_invalid;
  volatile unsigned long tx_frames;
  uint8_t sequence;
  int reg_status;
  int signal_strength;
  int lte_rsrp_dbm;
  int lte_rsrq_index;
  int sim_slot;
  bool sim_ready;
  bool profile_applied;
  bool keep_sim_unlocked;
  int sim_status;
  struct metalio_sim_profile_s profile;
  FAR char *at_response;
  size_t at_capacity;
  bool sms_prompt;
  bool at_overflow;
  FAR uint8_t *rx_stream;
  size_t rx_length;
  FAR struct nt26_rx_packet_s *rx_head;
  FAR struct nt26_rx_packet_s *rx_tail;
  FAR struct nt26_tx_packet_s *tx_head;
  FAR struct nt26_tx_packet_s *tx_tail;
  unsigned int tx_count;
  FAR uint8_t *tx_buffer;
  volatile clock_t last_activity;
};

static struct nt26_state_s g_nt26;
static bool g_nt26_created;
static atomic_bool g_radio_enabled;
static atomic_bool g_sms_active;
static atomic_bool g_sms_waiting;
static atomic_bool g_sms_cancel;
static atomic_uint g_sms_revision;
static atomic_bool g_sms_submitting;
static atomic_int g_call_state = METALIO_CALL_IDLE;
static atomic_bool g_call_release_requested = false;
/* Serialize the analog selector with local playback. The modem owns it
 * before ATD/ATA returns, when CLCC may still report idle/incoming. */
static mutex_t g_call_audio_lock = NXMUTEX_INITIALIZER;
static bool g_call_audio_selected;
static char g_call_urc_line[256];
static size_t g_call_urc_used;
static bool g_call_urc_overflow;
static clock_t g_clip_checked;
static bool g_clip_checked_once;
static int g_sim_probe_result=-EAGAIN;
static char g_pin_attempted_id[METALIO_SIM_ID_MAX];

static void nt26_update_call_state(int state)
{
  if (atomic_exchange(&g_call_state, state) != state)
    metalio_app_call_state_changed(state);
}

/* UART frames are transport chunks, not AT lines. CLIP/CLCC/RING can span
 * two frames; keep a bounded line accumulator independent of command replies. */
static void nt26_call_line(const char *line)
{
  while (*line == ' ' || *line == '\t') line++;
  if (strncmp(line, "+CMTI:", 6) == 0 || strncmp(line, "+CMT:", 5) == 0)
    atomic_fetch_add(&g_sms_revision, 1);
  const char *number = NULL;
  if (strncmp(line, "+CLIP:", 6) == 0) number = line + 6;
  else if (strncmp(line, "+CLCC:", 6) == 0)
    {
      number = line + 6;
      for (int field = 0; field < 5 && number != NULL; ++field)
        { number = strchr(number, ','); if (number != NULL) number++; }
    }
  if (number != NULL)
    {
      while (*number == ' ' || *number == '\t') number++;
      bool quoted = *number == '"'; if (quoted) number++;
      char digits[25]; size_t count = 0;
      while (*number && *number != '"' && *number != ',' && count < sizeof(digits) - 1)
        {
          if ((*number >= '0' && *number <= '9') || (*number == '+' && count == 0))
            digits[count++] = *number;
          else if (*number != ' ' && *number != '-') break;
          number++;
        }
      digits[count] = '\0';
      bool complete = quoted ? *number == '"' : *number == ',' || *number == '\0';
      if (complete && count > 0)
        {
          const char *type = quoted ? number + 1 : number;
          while (*type == ' ' || *type == '\t') type++;
          if (*type == ',' && atoi(type + 1) == 145 && digits[0] != '+' && count < sizeof(digits) - 1)
            { memmove(digits + 1, digits, count + 1); digits[0] = '+'; }
          metalio_app_call_number_changed(digits);
        }
    }
  if ((strcmp(line, "RING") == 0 || strncmp(line, "+CRING:", 7) == 0) &&
      atomic_load(&g_call_state) != METALIO_CALL_ACTIVE &&
      atomic_load(&g_call_state) != METALIO_CALL_HELD)
    nt26_update_call_state(METALIO_CALL_INCOMING);
  if (strcmp(line, "NO CARRIER") == 0 || strcmp(line, "NO ANSWER") == 0 ||
      strcmp(line, "BUSY") == 0)
    nt26_update_call_state(METALIO_CALL_IDLE);
}

static void nt26_feed_call_urc(const uint8_t *data, size_t len)
{
  for (size_t i = 0; i < len; ++i)
    {
      char c = (char)data[i];
      if (c == '\r' || c == '\n' || c == '\0')
        {
          if (g_call_urc_used && !g_call_urc_overflow)
            { g_call_urc_line[g_call_urc_used] = '\0'; nt26_call_line(g_call_urc_line); }
          g_call_urc_used = 0; g_call_urc_overflow = false;
        }
      else if (g_call_urc_used < sizeof(g_call_urc_line) - 1)
        g_call_urc_line[g_call_urc_used++] = c;
      else g_call_urc_overflow = true;
    }
}

static atomic_bool g_radio_allowed = true;
static mutex_t g_nt26_init_lock = NXMUTEX_INITIALIZER;

static int nt26_send_at(const char *command, char *response,
                        size_t response_size, unsigned int timeout_ms);

/* ICCID remains private: use it only as the per-card settings key. */
static int nt26_read_sim_identity(char id[METALIO_SIM_ID_MAX])
{
  char response[128];
  const char *commands[] = {"AT+CCID", "AT+ICCID"};
  id[0] = '\0';
  for (unsigned int i = 0; i < 2; i++)
    {
      if (nt26_send_at(commands[i], response, sizeof(response), 2000) < 0)
        continue;
      const char *p = strchr(response, ':');
      if (p == NULL) p = response;
      for (; *p != '\0'; p++)
        {
          if (*p < '0' || *p > '9') continue;
          const char *begin = p;
          while (*p >= '0' && *p <= '9') p++;
          size_t n = p - begin;
          if (n >= 18 && n < METALIO_SIM_ID_MAX)
            {
              memcpy(id, begin, n);
              id[n] = '\0';
              return OK;
            }
          if (*p == '\0') break;
        }
    }
  /* Some modem builds restrict CCID until PIN entry, although EF_ICCID
   * itself is readable without PIN1. Read only that standard SIM file. */
  if (nt26_send_at("AT+CRSM=176,12258,0,0,10", response, sizeof(response), 2500) == OK)
    {
      const char *field = strstr(response, "+CRSM:");
      unsigned sw1 = 0, sw2 = 0;
      char bcd[21] = {0};
      if (field && sscanf(field, "+CRSM: %u,%u,\"%20[0-9A-Fa-f]\"", &sw1, &sw2, bcd) == 3 &&
          sw1 == 144 && sw2 == 0 && strlen(bcd) == 20)
        {
          size_t n = 0;
          for (int i = 0; i < 20; i += 2)
            for (int j = 1; j >= 0; j--)
              {
                char digit = bcd[i + j];
                if (digit == 'F' || digit == 'f') continue;
                if (digit < '0' || digit > '9') { id[0] = 0; return -ENODATA; }
                id[n++] = digit;
              }
          id[n] = 0;
          if (n >= 18) return OK;
        }
    }
  return -ENODATA;
}

static bool nt26_check_sim_ready(void)
{
  char response[128] = {0};
  int ret = nt26_send_at("AT+CPIN?", response, sizeof(response), 2000);
  const char *pin = strstr(response, "+CPIN:");
  if (pin) { pin += 6; while (*pin == ' ' || *pin == '\t') pin++; }
  bool ready = ret == OK && pin != NULL && strncmp(pin, "READY", 5) == 0;
  int status = METALIO_SIM_UNKNOWN;
  if (ready) status = METALIO_SIM_READY;
  else if (strstr(response, "SIM PIN") || strstr(response, "SIM PUK"))
    status = METALIO_SIM_PIN;
  else if (strstr(response, "NOT INSERTED") ||
           strstr(response, "+CME ERROR: 10\r") ||
           strstr(response, "+CME ERROR: 10\n"))
    status = METALIO_SIM_ABSENT;
  g_sim_probe_result=ready?OK:status==METALIO_SIM_PIN?-EACCES:
      status==METALIO_SIM_ABSENT?-ENODEV:ret<0?ret:-EAGAIN;
  /* A lost AT response is not evidence that a working SIM was removed.
   * Preserve the last confirmed identity/readiness on transport errors. */
  if(ready)g_pin_attempted_id[0]=0;
  if(status!=METALIO_SIM_UNKNOWN){
    nxmutex_lock(&g_nt26.response_lock);
    g_nt26.sim_ready=ready;g_nt26.sim_status=status;
    if(status==METALIO_SIM_PIN||status==METALIO_SIM_ABSENT)g_nt26.keep_sim_unlocked=false;
    nxmutex_unlock(&g_nt26.response_lock);
  }
  return ready;
}

static bool nt26_try_remembered_pin(void)
{
  char response[128] = {0}, id[METALIO_SIM_ID_MAX] = {0};
  char pin[9] = {0}, command[32] = {0};
  if(nt26_send_at("AT+CPIN?",response,sizeof(response),2000)<0)return false;
  const char *required=strstr(response,"+CPIN:");
  if(!required)return false;required+=6;while(*required==' '||*required=='\t')++required;
  if(strncmp(required,"SIM PIN",7)!=0)return false;
  /* Never fall back to the slot number: another SIM may occupy that slot. */
  if (nt26_read_sim_identity(id) < 0 ||
      metalio_app_sim_pin_read(id, pin, sizeof(pin)) < 0) return false;
  if(strcmp(g_pin_attempted_id,id)==0){
    for(volatile char *p=pin;p<pin+sizeof(pin);p++)*p=0;
    return false;
  }
  strlcpy(g_pin_attempted_id,id,sizeof(g_pin_attempted_id));
  snprintf(command,sizeof(command),"AT+CPIN=\"%s\"",pin);
  int unlocked=nt26_send_at(command,response,sizeof(response),5000);
  bool rejected=unlocked<0&&(strstr(response,"+CME ERROR: 16\r")||
      strstr(response,"+CME ERROR: 16\n")||strstr(response,"incorrect password"));
  for(volatile char *p=pin;p<pin+sizeof(pin);p++)*p=0;
  for(volatile char *p=command;p<command+sizeof(command);p++)*p=0;
  for(volatile char *p=response;p<response+sizeof(response);p++)*p=0;
  if(rejected){metalio_app_sim_pin_forget(id);return false;}
  for(int n=0;n<12;n++){
    if(nt26_check_sim_ready()){g_nt26.keep_sim_unlocked=true;g_pin_attempted_id[0]=0;return true;}
    usleep(250000);
  }
  /* A transport timeout never deletes a previously verified PIN, and the
   * same card is not retried until its PIN session is confirmed or changed. */

  return false;
}

static bool nt26_preserve_unlocked_sim(void)
{
  if (g_nt26.keep_sim_unlocked) return true;
  if (!nt26_check_sim_ready()) return false;
  char response[128];
  int enabled = 0;
  int ret = nt26_send_at("AT+CLCK=\"SC\",2", response, sizeof(response), 2500);
  const char *state = strstr(response, "+CLCK:");
  if (ret == OK && state && sscanf(state, "+CLCK: %d", &enabled) == 1 && enabled == 1)
    g_nt26.keep_sim_unlocked = true;
  return g_nt26.keep_sim_unlocked;
}

static void nt26_invalidate_sim(void)
{
  if (atomic_load(&g_call_state) != METALIO_CALL_IDLE)
    { nt26_update_call_state(METALIO_CALL_IDLE); (void)metalio_nt26_set_call_audio_path(false); }
  g_nt26.initialized = false;
  g_nt26.profile_applied = false;
  g_nt26.carrier = false;
  netdev_lower_carrier_off(&g_nt26.dev);
  nxmutex_lock(&g_nt26.response_lock);
  /* A confirmed removed/replaced SIM must not lend its identity to SMS
   * while a new card is becoming ready. APN application is separate. */
  g_nt26.profile.iccid[0] = '\0';
  g_nt26.reg_status = 0;
  g_nt26.signal_strength = 99;
  g_nt26.lte_rsrp_dbm = -999;
  nxmutex_unlock(&g_nt26.response_lock);
}

static uint8_t nt26_header_checksum(const uint8_t *h)
{
  uint32_t sum = h[0] + h[1] + h[2];
  return (uint8_t)((sum >> 8) ^ sum ^ 0x03);
}

static void nt26_make_header(uint8_t *h, uint16_t len, uint8_t seq,
                             uint8_t type)
{
  h[0] = len & 0xff;
  h[1] = ((seq & 0x0f) << 4) | ((len >> 8) & 0x0f);
  h[2] = (uint8_t)((type & 0x03) << 2); /* XON, final frame */
  h[3] = nt26_header_checksum(h);
}

static bool nt26_header_valid(const uint8_t *h)
{
  uint16_t len=h[0]|((h[1]&0x0f)<<8);
  unsigned type=(h[2]>>2)&3;
  return h[3]==nt26_header_checksum(h)&&(h[2]&0xf0)==0&&
         type<=NT26_FRAME_AT&&len<=NT26_MAX_PAYLOAD&&
         /* SMS entry may return just the single-byte '>' prompt. Keep
          * normal Ethernet/AT resynchronization limits unchanged. */
         (type==NT26_FRAME_AT?(len>=2||(len==1&&atomic_load(&g_sms_active))):(len>=14||len==6));
}

static int nt26_uart_write(const uint8_t *data, size_t len)
{
  size_t sent = 0;
  clock_t start = clock_systime_ticks();
  while (sent < len)
    {
      if (clock_systime_ticks() - start >= MSEC2TICK(1000))
        {
          return -ETIMEDOUT;
        }
      ssize_t n = write(g_nt26.uart_fd, data + sent, len - sent);
      if (n > 0)
        {
          sent += n;
        }
      else if (n < 0 && errno == EINTR)
        {
          continue;
        }
      else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
          usleep(1000);
        }
      else
        {
          return n < 0 ? -errno : -EIO;
        }
    }

  return OK;
}

/* SRDY ACK is a short rising edge. Polling it once per scheduler tick
 * misses the edge and adds 100 ms to every Ethernet packet, slower than
 * the 60 ms voice frames. Capture it in the GPIO ISR as the reference does. */
static int nt26_ack_interrupt(int irq, void *context, void *arg)
{
  (void)irq;
  (void)context;
  (void)arg;
  if (g_nt26.tx_active) nxsem_post(&g_nt26.tx_ack);
  return OK;
}

/* MRDY/SRDY are active-low busy handshakes, matching the reference driver. */
static int nt26_send_frame(const uint8_t *payload, size_t len, uint8_t type)
{
  FAR uint8_t *frame = g_nt26.tx_buffer;
  int ret = OK;

  if (len > NT26_MAX_PAYLOAD)
    {
      return -E2BIG;
    }
  if (frame == NULL)
    {
      return -ENODEV;
    }

  nxmutex_lock(&g_nt26.tx_lock);
  g_nt26.tx_active = true;
  g_nt26.last_activity = clock_systime_ticks();
  nt26_make_header(frame, len, g_nt26.sequence++, type);
  memcpy(frame + 4, payload, len);
  /* Ask the modem to wake, then wait briefly for its active-low ready line. */
  esp_gpiowrite(BOARD_NT26_MRDY_GPIO, false);
  clock_t wake_start = clock_systime_ticks();
  while (esp_gpioread(BOARD_NT26_SRDY_GPIO) &&
         clock_systime_ticks() - wake_start < MSEC2TICK(200))
    {
      usleep(1000);
    }

  if (esp_gpioread(BOARD_NT26_SRDY_GPIO))
    {
      /* The reference modem permits a framed AT probe after a missed wake
       * edge. A validated response, not this GPIO, decides success. */
      syslog(LOG_WARNING, "NT26: SRDY wake timeout; probing framed UART\n");
    }

  while (nxsem_trywait(&g_nt26.tx_ack) == OK) { }
  ret = nt26_uart_write(frame, len + 4);
  if (ret < 0)
    {
      goto out;
    }

  /* Ensure the framed bytes have left the UART before starting the bounded
   * modem acknowledgement window. */
  ret = tcdrain(g_nt26.uart_fd);
  if (ret < 0)
    {
      ret = -errno;
      goto out;
    }

  /* The IRQ retains ACK even when SRDY has gone low again before this
   * thread runs. Timeout remains best-effort; TCP/AT owns delivery checks. */
  (void)nxsem_tickwait(&g_nt26.tx_ack, MSEC2TICK(100));

out:
  g_nt26.tx_frames++;
  g_nt26.last_activity = clock_systime_ticks();
  /* Keep MRDY asserted while the modem remains active, as in the reference
   * state machine. Release it only after a failed transfer when the modem is
   * already idle; the RX worker owns modem-initiated wakeups. */
  if (ret < 0 && esp_gpioread(BOARD_NT26_SRDY_GPIO))
    {
      esp_gpiowrite(BOARD_NT26_MRDY_GPIO, true);
    }
  g_nt26.tx_active = false;
  nxmutex_unlock(&g_nt26.tx_lock);
  return ret;
}

static bool nt26_sms_response_complete(const char *response)
{
  if (g_nt26.sms_prompt && strchr(response, '>') != NULL) return true;
  bool reference_seen = false;
  for (const char *line = response; *line; )
    {
      while (*line == '\r' || *line == '\n') ++line;
      const char *end = strpbrk(line, "\r\n");
      size_t len = end ? (size_t)(end - line) : strlen(line);
      if (end && len > 6 && !strncmp(line, "+CMGS:", 6)) {
        size_t at=6;while(at<len && (line[at]==' '||line[at]=='\t'))++at;
        const size_t digits=at;unsigned reference=0;
        while(at<len && isdigit((unsigned char)line[at])) {
          if(reference>255){reference=256;break;}
          reference=reference*10+(unsigned)(line[at++]-'0');
        }
        const bool number=at>digits;
        while(at<len && (line[at]==' '||line[at]=='\t'))++at;
        if(number && at==len && reference<=255)reference_seen=true;
      }
      /* The UART bridge may acknowledge command dispatch before reporting
       * the network message reference. Keep collecting until CMGS + OK;
       * a dispatch-only OK is not a successful SMS submission. */
      if ((len == 2 && !strncmp(line, "OK", 2) &&
           (!atomic_load(&g_sms_submitting) || reference_seen)) ||
          (len == 5 && !strncmp(line, "ERROR", 5)) ||
          (len == 4 && !strncmp(line, "BUSY", 4)) ||
          (len == 10 && !strncmp(line, "NO CARRIER", 10)) ||
          (len == 9 && !strncmp(line, "NO ANSWER", 9))) return true;
      /* An ERROR token can arrive before its numeric code in another
       * UART frame. Wait for the complete line before publishing failure. */
      if (end && ((!strncmp(line, "+CMS ERROR:", 11)) ||
                  (!strncmp(line, "+CME ERROR:", 11)))) return true;
      if (!end) break;
      line = end + 1;
    }
  return false;
}

static void nt26_dispatch_frame(uint8_t *frame, size_t frame_size)
{
  if (frame_size < 4) return;
  uint16_t len = (uint16_t)frame[0] | ((uint16_t)(frame[1] & 0x0f) << 8);
  uint8_t type = (frame[2] >> 2) & 0x03;
  FAR uint8_t *payload = frame + 4;
  if (frame_size < 4 || (size_t)len + 4 > frame_size)
    {
      return;
    }

  if (type == NT26_FRAME_AT)
    {
      char text[768];
      size_t text_len = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
      memcpy(text, payload, text_len);
      text[text_len] = '\0';
      nt26_feed_call_urc(payload, len);
      char *reg = strstr(text, "+CEREG:");
      if (reg != NULL)
        {
          int first = -1;
          int second = -1;
          nxmutex_lock(&g_nt26.response_lock);
          if (sscanf(reg, "+CEREG: %d,%d", &first, &second) == 2)
            {
              g_nt26.reg_status = second;
            }
          else if (sscanf(reg, "+CEREG: %d", &first) == 1)
            {
              g_nt26.reg_status = first;
            }
          nxmutex_unlock(&g_nt26.response_lock);
        }
      char *csq = strstr(text, "+CSQ:");
      if (csq != NULL)
        {
          int strength = 99;
          if (sscanf(csq, "+CSQ: %d", &strength) == 1 &&
              strength >= 0 && strength <= 31)
            {
              nxmutex_lock(&g_nt26.response_lock);
              g_nt26.signal_strength = strength;
              nxmutex_unlock(&g_nt26.response_lock);
            }
          else
            {
              nxmutex_lock(&g_nt26.response_lock);
              g_nt26.signal_strength = 99;
              nxmutex_unlock(&g_nt26.response_lock);
            }
        }
      char *cesq = strstr(text, "+CESQ:");
      if (cesq != NULL)
        {
          int rxlev, ber, rscp, ecno, rsrq, rsrp;
          if (sscanf(cesq, "+CESQ: %d,%d,%d,%d,%d,%d",
                     &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp) == 6)
            {
              nxmutex_lock(&g_nt26.response_lock);
              g_nt26.lte_rsrp_dbm = rsrp >= 0 && rsrp <= 97
                                      ? rsrp - 140 : -999;
              g_nt26.lte_rsrq_index = rsrq >= 0 && rsrq <= 34
                                        ? rsrq : -1;
              nxmutex_unlock(&g_nt26.response_lock);
            }
        }

      nxmutex_lock(&g_nt26.response_lock);
      if (g_nt26.waiting_at)
        {
          size_t have = strlen(g_nt26.at_response);
          size_t copy = g_nt26.at_capacity > have ? g_nt26.at_capacity - have - 1 : 0;
          if (copy < len) g_nt26.at_overflow = true;
          if (copy > len)
            {
              copy = len;
            }
          if (copy > 0)
            {
              memcpy(g_nt26.at_response + have, payload, copy);
              g_nt26.at_response[have + copy] = '\0';
            }
          bool sms = atomic_load(&g_sms_active);
          if ((sms && nt26_sms_response_complete(g_nt26.at_response)) ||
              (!sms && ((g_nt26.sms_prompt && strchr(g_nt26.at_response, '>') != NULL) ||
              strstr(g_nt26.at_response, "\r\nOK") != NULL ||
              strstr(g_nt26.at_response, "\nOK") != NULL ||
              strstr(g_nt26.at_response, "ERROR") != NULL ||
              strstr(g_nt26.at_response, "NO CARRIER") != NULL ||
              strstr(g_nt26.at_response, "BUSY") != NULL ||
              strstr(g_nt26.at_response, "NO ANSWER") != NULL)))
            {
              /* Only CMGL loans the large inbox buffer. Its owner must
               * retain reception past a bridge dispatch-only OK, until the
               * actual PDU list plus its terminal result has arrived. Other
               * AT/call/network transactions keep their existing ownership. */
              if (!(sms && g_nt26.at_capacity > 4096))
                g_nt26.waiting_at = false;
              nxsem_post(&g_nt26.at_done);
            }
        }
      nxmutex_unlock(&g_nt26.response_lock);
      return;
    }

  if (type != NT26_FRAME_ETH)
    {
      return;
    }
  static const uint8_t ack[] = {0x53, 0x50, 0x49, 0x43, 0x01, 0x80};
  if (!g_nt26.handshake_seen && len >= sizeof(ack) &&
      memcmp(payload, ack, sizeof(ack)) == 0)
    {
      g_nt26.handshake_seen = true;
      g_nt26.carrier = true;
      nxsem_post(&g_nt26.handshake_done);
      netdev_lower_carrier_on(&g_nt26.dev);
      syslog(LOG_INFO, "NT26: UART Ethernet handshake complete\n");
      return;
    }

  if (!g_nt26.carrier || len < 14 || len > NT26_MAX_PAYLOAD)
    {
      return;
    }

  FAR netpkt_t *pkt = netpkt_alloc(&g_nt26.dev, NETPKT_RX);
  FAR struct nt26_rx_packet_s *node = kmm_malloc(sizeof(*node));
  if (pkt == NULL || node == NULL)
    {
      if (pkt != NULL)
        {
          netpkt_free(&g_nt26.dev, pkt, NETPKT_RX);
        }
      kmm_free(node);
      return;
    }

  if (netpkt_copyin(&g_nt26.dev, pkt, payload, len, 0) < 0)
    {
      netpkt_free(&g_nt26.dev, pkt, NETPKT_RX);
      kmm_free(node);
      return;
    }

  node->flink = NULL;
  node->pkt = pkt;
  nxmutex_lock(&g_nt26.rx_lock);
  if (g_nt26.rx_tail != NULL)
    {
      g_nt26.rx_tail->flink = node;
    }
  else
    {
      g_nt26.rx_head = node;
    }
  g_nt26.rx_tail = node;
  nxmutex_unlock(&g_nt26.rx_lock);
  netdev_lower_rxready(&g_nt26.dev);
}

static void nt26_process_rx(void)
{
  size_t offset = 0;
  while (g_nt26.rx_length - offset >= 4)
    {
      FAR uint8_t *h = g_nt26.rx_stream + offset;
      if (!nt26_header_valid(h))
        {
          g_nt26.rx_invalid++;
          offset++;
          continue;
        }
      uint16_t len = (uint16_t)h[0] | ((uint16_t)(h[1] & 0x0f) << 8);
      size_t frame_size = 4u + len;
      if (len > NT26_MAX_PAYLOAD)
        {
          offset++;
          continue;
        }
      if (g_nt26.rx_length - offset < frame_size)
        {
          break;
        }

      nt26_dispatch_frame(h, frame_size);
      g_nt26.rx_frames++;
      /* Acknowledge every complete frame, including unsolicited traffic. */
      esp_gpiowrite(BOARD_NT26_MRDY_GPIO, true);
      esp_rom_delay_us(50);
      esp_gpiowrite(BOARD_NT26_MRDY_GPIO, false);
      offset += frame_size;
    }

  if (offset > 0)
    {
      g_nt26.rx_length -= offset;
      if (g_nt26.rx_length > 0)
        {
          memmove(g_nt26.rx_stream, g_nt26.rx_stream + offset,
                  g_nt26.rx_length);
        }
    }
}

/* UART runs at 2 Mbaud: even the largest frame takes under 12 ms.
 * A truncated header/payload must not consume all later AT/DNS/TLS replies
 * while waiting forever for bytes from a frame that has already been lost.
 */
static void nt26_drop_stale_rx(clock_t last_rx)
{
  if (g_nt26.rx_length > 0 &&
      clock_systime_ticks() - last_rx >= MSEC2TICK(250))
    {
      syslog(LOG_WARNING, "NT26: discard stale partial frame bytes=%u\n",
             (unsigned)g_nt26.rx_length);
      /* An interrupted payload may already contain a complete later AT
       * or Ethernet frame. Resynchronise it instead of throwing it away
       * with the incomplete leading frame. */
      g_nt26.rx_invalid++;
      --g_nt26.rx_length;
      memmove(g_nt26.rx_stream,g_nt26.rx_stream+1,g_nt26.rx_length);
      nt26_process_rx();
      if(g_nt26.rx_length){g_nt26.rx_invalid+=g_nt26.rx_length;g_nt26.rx_length=0;}
    }
}

/* task_create clones the caller's descriptor table. A permanent modem
 * task must not retain an OTA/MQTT socket from its creator after that
 * client closes it, or the fixed TCP pool eventually cannot reconnect.
 * These workers own only the UART; release inherited sockets in their
 * separate task group without touching the parent's devices or sockets. */
static void nt26_release_inherited_sockets(void)
{
  long limit = sysconf(_SC_OPEN_MAX);
  int released = 0;
  if (limit < 0 || limit > 1024) limit = 1024;
  for (int fd = 3; fd < limit; ++fd)
    {
      int type;
      socklen_t length = sizeof(type);
      if (fd != g_nt26.uart_fd &&
          getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0)
        {
          close(fd);
          released++;
        }
    }
  if (released)
    syslog(LOG_INFO, "NT26: released inherited sockets=%d\n", released);
}

static int nt26_rx_worker(int argc, FAR char *argv[])
{
  nt26_release_inherited_sockets();
  uint8_t buf[768];
  clock_t last_rx = clock_systime_ticks();
  (void)argc;
  (void)argv;

  while (g_nt26.uart_fd >= 0)
    {
      /* SRDY low is also a modem-initiated wake request. Without this
       * response, downlink frames wait forever while MRDY stays idle. */
      if (!g_nt26.tx_active && !esp_gpioread(BOARD_NT26_SRDY_GPIO))
        {
          esp_gpiowrite(BOARD_NT26_MRDY_GPIO, false);
        }
      ssize_t n = read(g_nt26.uart_fd, buf, sizeof(buf));
      if (n > 0)
        {
          last_rx = clock_systime_ticks();
          g_nt26.rx_bytes += n;
          g_nt26.last_activity = clock_systime_ticks();
          size_t copy = (size_t)n;
          if (copy > NT26_RX_STREAM_SIZE - g_nt26.rx_length)
            {
              /* Drop malformed/overrun stream and resynchronise at header. */
              g_nt26.rx_length = 0;
              copy = copy > NT26_RX_STREAM_SIZE ? NT26_RX_STREAM_SIZE : copy;
            }
          memcpy(g_nt26.rx_stream + g_nt26.rx_length, buf, copy);
          g_nt26.rx_length += copy;
          nt26_process_rx();
        }
      else
        {
          /* Drain bytes already buffered by UART before expiring a partial
           * frame: flash/IRQ stalls may delay the reader, not the modem. */
          nt26_drop_stale_rx(last_rx);
          if (esp_gpioread(BOARD_NT26_SRDY_GPIO) &&
              clock_systime_ticks() - g_nt26.last_activity >= MSEC2TICK(500) &&
              nxmutex_trylock(&g_nt26.tx_lock) == OK)
            {
              esp_gpiowrite(BOARD_NT26_MRDY_GPIO, true);
              nxmutex_unlock(&g_nt26.tx_lock);
            }
          usleep(2000);
        }
    }
  return OK;
}

static int nt26_signal_worker(int argc, FAR char *argv[])
{
  nt26_release_inherited_sockets();
  char response[128];
  (void)argc;
  (void)argv;

  while (g_nt26.control_ready)
    {
      /* CFUN/SIM changes are a multi-command transaction. Do not inject
       * signal queries between radio-off, slot selection and radio-on. */
      nxmutex_lock(&g_nt26_init_lock);
      /* Let the queued inbox transaction acquire AT between telemetry
       * cycles. Real call URCs and urgent hangup remain independent. */
      if (atomic_load(&g_sms_waiting) || atomic_load(&g_sms_active))
        {
          nxmutex_unlock(&g_nt26_init_lock);
          usleep(100000);
          continue;
        }
      if (!atomic_load(&g_radio_allowed) || !atomic_load(&g_radio_enabled))
        {
          nxmutex_unlock(&g_nt26_init_lock);
          usleep(2000000);
          continue;
        }
      const bool was_ready = g_nt26.sim_ready;
      const bool was_initialized = g_nt26.initialized;
      if (!nt26_check_sim_ready())
        {
          if(g_sim_probe_result!=-ENODEV&&g_sim_probe_result!=-EACCES){
            /* Keep a working data path on a transient query loss. */
            nxmutex_unlock(&g_nt26_init_lock);usleep(3000000);continue;
          }
          if(g_sim_probe_result==-EACCES&&nt26_try_remembered_pin()){
            nxmutex_unlock(&g_nt26_init_lock);metalio_app_cellular_sim_changed(true);usleep(3000000);continue;
          }
          if(g_sim_probe_result==-ENODEV){
            usleep(350000);
            if(nt26_check_sim_ready()||g_sim_probe_result!=-ENODEV){nxmutex_unlock(&g_nt26_init_lock);usleep(3000000);continue;}
          }
          nt26_invalidate_sim();
          nxmutex_unlock(&g_nt26_init_lock);
          if (was_ready || was_initialized)
            metalio_app_cellular_sim_changed(false);
          usleep(3000000);
          continue;
        }
      char current_id[METALIO_SIM_ID_MAX];
      bool known_id = nt26_read_sim_identity(current_id) == OK;
      /* WiFi standby has no initialized data path/APN. An empty identity
       * is not a SIM swap and must not terminate a valid call. Remember the
       * observed card without marking its data profile as applied. */
      bool changed = known_id && g_nt26.profile.iccid[0] &&
                     strcmp(current_id, g_nt26.profile.iccid) != 0;
      bool recover_data = !was_ready || !g_nt26.initialized || changed;
      if (changed || (!was_ready && was_initialized))
        {
          if (changed) g_nt26.keep_sim_unlocked = false;
          nt26_invalidate_sim();
        }
      if (known_id && (!g_nt26.profile.iccid[0] || changed))
        {
          nxmutex_lock(&g_nt26.response_lock);
          strlcpy(g_nt26.profile.iccid, current_id, sizeof(g_nt26.profile.iccid));
          g_nt26.profile.slot = g_nt26.sim_slot;
          nxmutex_unlock(&g_nt26.response_lock);
        }
      /* The RX parser applies confirmed registration reports. A missing
       * reply alone must not replace a registered state with no service. */
      (void)nt26_send_at("AT+CEREG?",response,sizeof(response),2500);
      (void)nt26_send_at("AT+CSQ", response, sizeof(response), 2500);
      response[0] = '\0';
      int ret = nt26_send_at("AT+CESQ", response, sizeof(response), 2500);
      int rxlev, ber, rscp, ecno, rsrq, rsrp;
      char *value = strstr(response, "+CESQ:");

      if (ret == OK && value != NULL &&
          sscanf(value, "+CESQ: %d,%d,%d,%d,%d,%d",
                 &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp) == 6)
        {
          if (rsrp >= 0 && rsrp <= 97)
            {
              syslog(LOG_INFO,
                     "NT26: LTE signal sim=%d RSRP=%d dBm RSRQ-index=%d\n",
                     g_nt26.sim_slot, rsrp - 140, rsrq);
            }
          else
            {
              syslog(LOG_INFO, "NT26: LTE signal sim=%d unknown\n",
                     g_nt26.sim_slot);
            }
        }
      else
        {
          int available = -1;
          (void)ioctl(g_nt26.uart_fd, FIONREAD, &available);
          syslog(LOG_WARNING, "NT26 stall ret=%d s=%d m=%d buf=%d part=%u q=%u active=%d\n",
                 ret, esp_gpioread(BOARD_NT26_SRDY_GPIO),
                 esp_gpioread(BOARD_NT26_MRDY_GPIO),
                 available, (unsigned)g_nt26.rx_length,
                 g_nt26.tx_count, g_nt26.tx_active);
          syslog(LOG_WARNING, "NT26 frames tx=%lu rx=%lu bytes=%lu bad=%lu\n",
                 g_nt26.tx_frames, g_nt26.rx_frames, g_nt26.rx_bytes,
                 g_nt26.rx_invalid);
        }
      nxmutex_unlock(&g_nt26_init_lock);
      if (recover_data) metalio_app_cellular_sim_changed(true);
      usleep(15000000);
    }

  return 0;
}

static int nt26_send_at_locked(const char *command, char *response, size_t response_size,
                               unsigned int timeout_ms)
{
  char cmd[160];
  size_t len = strlen(command);
  int ret;

  if (len + 2 > sizeof(cmd))
    {
      return -E2BIG;
    }

  while (nxsem_trywait(&g_nt26.at_done) == OK)
    {
    }

  memcpy(cmd, command, len);
  if (len == 0 || cmd[len - 1] != '\r')
    {
      cmd[len++] = '\r';
    }
  cmd[len] = '\0';

  nxmutex_lock(&g_nt26.response_lock);
  g_nt26.at_response[0] = '\0';
  g_nt26.at_overflow = false;
  g_nt26.waiting_at = true;
  nxmutex_unlock(&g_nt26.response_lock);

  ret = nt26_send_frame((const uint8_t *)cmd, len, NT26_FRAME_AT);
  if (ret == OK)
    {
      ret = nxsem_tickwait(&g_nt26.at_done, MSEC2TICK(timeout_ms));
      if (ret < 0)
        {
          ret = -ETIMEDOUT;
        }
    }

  nxmutex_lock(&g_nt26.response_lock);
  g_nt26.waiting_at = false;
  if (response != NULL && response_size > 0)
    {
      strlcpy(response, g_nt26.at_response, response_size);
    }
  nxmutex_unlock(&g_nt26.response_lock);

  if (ret == OK && g_nt26.at_overflow)
    {
      ret = -EOVERFLOW;
    }
  else if (ret == OK && strstr(g_nt26.at_response, "ERROR") != NULL)
    {
      ret = -EIO;
    }
  else if (ret == OK &&
           (strstr(g_nt26.at_response, "NO CARRIER") != NULL ||
            strstr(g_nt26.at_response, "NO ANSWER") != NULL))
    {
      ret = -ENOTCONN;
    }
  else if (ret == OK && strstr(g_nt26.at_response, "BUSY") != NULL)
    {
      ret = -EBUSY;
    }
  if (ret == OK) {
    if (strcmp(command, "AT+CFUN=1") == 0) atomic_store(&g_radio_enabled, true);
    else if (strcmp(command, "AT+CFUN=0") == 0) {
      atomic_store(&g_radio_enabled, false);
      g_nt26.keep_sim_unlocked = false;
    }
    else if (strcmp(command, "AT+CFUN=4") == 0) atomic_store(&g_radio_enabled, false);
  }
  return ret;
}

static int nt26_send_at(const char *command, char *response, size_t response_size,
                        unsigned int timeout_ms)
{
  nxmutex_lock(&g_nt26.at_lock);
  int ret = nt26_send_at_locked(command, response, response_size, timeout_ms);
  nxmutex_unlock(&g_nt26.at_lock);
  return ret;
}

static int nt26_lock_call_urgent(void)
{
  for (int waited = 0; nxmutex_trylock(&g_nt26.at_lock) < 0; waited += 20)
    {
      if(atomic_load(&g_sms_active))atomic_store(&g_sms_cancel,true);
      if (waited >= 3000) return -EBUSY;
      usleep(20000);
    }
  return OK;
}

static void nt26_confirm_caller_reporting(void)
{
  char response[128]; int enabled = 0;
  int ret = nt26_send_at("AT+CLIP?", response, sizeof(response), 700);
  const char *value = strstr(response, "+CLIP:");
  if (ret != OK || value == NULL || sscanf(value, "+CLIP: %d", &enabled) != 1 || enabled != 1)
    {
      enabled = 0;
      ret = nt26_send_at("AT+CLIP=1", response, sizeof(response), 700);
      if (ret == OK) ret = nt26_send_at("AT+CLIP?", response, sizeof(response), 700);
      value = strstr(response, "+CLIP:");
      if (ret != OK || value == NULL || sscanf(value, "+CLIP: %d", &enabled) != 1) enabled = 0;
    }
  g_clip_checked = clock_systime_ticks(); g_clip_checked_once = true;
  syslog(LOG_INFO, "CALL_CLIP_%s\n", enabled == 1 ? "READY" : "UNCONFIRMED");
}

static int nt26_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct nt26_state_s *priv = (FAR struct nt26_state_s *)dev;
  if (priv->carrier)
    {
      netdev_lower_carrier_on(dev);
    }
  return OK;
}

static int nt26_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  netdev_lower_carrier_off(dev);
  return OK;
}

static int nt26_transmit(FAR struct netdev_lowerhalf_s *dev,
                         FAR netpkt_t *pkt)
{
  FAR struct nt26_tx_packet_s *node;
  FAR struct nt26_tx_packet_s *prev;
  FAR struct nt26_tx_packet_s *next;
  unsigned int len = netpkt_getdatalen(dev, pkt);
  int ret;

  if (!g_nt26.carrier)
    {
      return -ENETDOWN;
    }
  if (len == 0 || len > NT26_MAX_PAYLOAD)
    {
      return -E2BIG;
    }

  /* The upper half holds the network lock here.  Only copy and enqueue;
   * never wait for UART/SRDY or the AT sender in this callback.  On error
   * the upper half retains pkt, so it MUST NOT be freed on that path. */

  nxmutex_lock(&g_nt26.tx_queue_lock);
  if (g_nt26.tx_count >= NT26_TX_QUEUE_DEPTH)
    {
      nxmutex_unlock(&g_nt26.tx_queue_lock);
      return -EBUSY;
    }

  node = kmm_malloc(sizeof(*node) + len);
  if (node == NULL)
    {
      nxmutex_unlock(&g_nt26.tx_queue_lock);
      return -ENOMEM;
    }

  /* netpkt_copyout returns bytes copied (despite its stale API comment). */

  ret = netpkt_copyout(dev, node->data, pkt, len, 0);
  if (ret != (int)len)
    {
      kmm_free(node);
      nxmutex_unlock(&g_nt26.tx_queue_lock);
      return ret < 0 ? ret : -EIO;
    }

  node->len = len;
  node->flink = NULL;
  /* Keep link resolution and TCP progress ahead of best-effort traffic on
   * this UART-backed cellular link.  Inserting after existing equal-priority
   * packets preserves TCP segment ordering. */
  if (len >= 14 && node->data[12] == 0x08 && node->data[13] == 0x06)
    {
      node->priority = 0; /* ARP */
    }
  else if (len >= 54 && node->data[12] == 0x08 &&
           node->data[13] == 0x00 && node->data[23] == 6)
    {
      node->priority = 1; /* TCP */
    }
  else
    {
      node->priority = 2; /* Best-effort traffic, including UDP */
    }

  prev = NULL;
  next = g_nt26.tx_head;
  while (next != NULL && next->priority <= node->priority)
    {
      prev = next;
      next = next->flink;
    }
  node->flink = next;
  if (prev != NULL)
    {
      prev->flink = node;
    }
  else
    {
      g_nt26.tx_head = node;
    }
  if (next == NULL)
    {
      g_nt26.tx_tail = node;
    }
  else if (prev == NULL && g_nt26.tx_tail == NULL)
    {
      g_nt26.tx_tail = node;
    }
  g_nt26.tx_count++;
  nxmutex_unlock(&g_nt26.tx_queue_lock);
  netpkt_free(dev, pkt, NETPKT_TX);
  nxsem_post(&g_nt26.tx_ready);
  return OK;
}

static int nt26_tx_worker(int argc, FAR char *argv[])
{
  nt26_release_inherited_sockets();
  FAR struct nt26_tx_packet_s *node;
  int ret;

  while (g_nt26.uart_fd >= 0)
    {
      if (nxsem_wait_uninterruptible(&g_nt26.tx_ready) < 0)
        {
          continue;
        }
      nxmutex_lock(&g_nt26.tx_queue_lock);
      node = g_nt26.tx_head;
      if (node != NULL)
        {
          g_nt26.tx_head = node->flink;
          if (g_nt26.tx_head == NULL)
            {
              g_nt26.tx_tail = NULL;
            }
          g_nt26.tx_count--;
        }
      nxmutex_unlock(&g_nt26.tx_queue_lock);
      if (node == NULL)
        {
          continue;
        }

      ret = nt26_send_frame(node->data, node->len, NT26_FRAME_ETH);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "NT26: Ethernet TX failed: %d\n", ret);
        }
      kmm_free(node);
      netdev_lower_txdone(&g_nt26.dev);
    }
  return OK;
}

static FAR netpkt_t *nt26_receive(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct nt26_rx_packet_s *node;
  FAR netpkt_t *pkt = NULL;

  nxmutex_lock(&g_nt26.rx_lock);
  node = g_nt26.rx_head;
  if (node != NULL)
    {
      g_nt26.rx_head = node->flink;
      if (g_nt26.rx_head == NULL)
        {
          g_nt26.rx_tail = NULL;
        }
      pkt = node->pkt;
      kmm_free(node);
    }
  nxmutex_unlock(&g_nt26.rx_lock);
  return pkt;
}

static const struct netdev_ops_s g_nt26_ops =
{
  .ifup = nt26_ifup,
  .ifdown = nt26_ifdown,
  .transmit = nt26_transmit,
  .receive = nt26_receive,
};

static int nt26_init_uart(void)
{
  struct termios tio;
  int ret;

  ret = esp_configgpio(BOARD_NT26_MRDY_GPIO, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }
  ret = esp_configgpio(BOARD_NT26_SRDY_GPIO, INPUT | PULLUP | RISING);
  if (ret < 0)
    {
      return ret;
    }
  esp_gpioirqdisable(BOARD_NT26_SRDY_GPIO);
  ret = esp_gpio_irq(BOARD_NT26_SRDY_GPIO, nt26_ack_interrupt, NULL);
  if (ret < 0) return ret;
  esp_gpioirqenable(BOARD_NT26_SRDY_GPIO);
  esp_gpiowrite(BOARD_NT26_MRDY_GPIO, true);

  g_nt26.uart_fd = open(NT26_UART_PATH, O_RDWR | O_NONBLOCK);
  if (g_nt26.uart_fd < 0)
    {
      return -errno;
    }
  if (tcgetattr(g_nt26.uart_fd, &tio) < 0)
    {
      ret = -errno;
      close(g_nt26.uart_fd);
      g_nt26.uart_fd = -1;
      return ret;
    }
  cfmakeraw(&tio);
  cfsetispeed(&tio, NT26_BAUD);
  cfsetospeed(&tio, NT26_BAUD);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (tcsetattr(g_nt26.uart_fd, TCSANOW, &tio) < 0)
    {
      ret = -errno;
      close(g_nt26.uart_fd);
      g_nt26.uart_fd = -1;
      return ret;
    }
  return OK;
}

static int nt26_register_netdev(void)
{
  int ret;
  memset(&g_nt26.dev, 0, sizeof(g_nt26.dev));
  g_nt26.dev.ops = &g_nt26_ops;
  /* Keep TCP/UDP processing off the UART drain task. This dedicated upper
   * half thread cannot block modem reads behind a network lock or LPWORK. */
  g_nt26.dev.rxtype = NETDEV_RX_THREAD;
  g_nt26.dev.priority = 100;
  g_nt26.dev.quota[NETPKT_TX] = CONFIG_IOB_NBUFFERS / 4;
  g_nt26.dev.quota[NETPKT_RX] = CONFIG_IOB_NBUFFERS / 4;
  g_nt26.dev.netdev.d_mac.ether.ether_addr_octet[0] = 0x02;
  g_nt26.dev.netdev.d_mac.ether.ether_addr_octet[1] = 0x26;
  g_nt26.dev.netdev.d_mac.ether.ether_addr_octet[2] = 0x4e;
  g_nt26.dev.netdev.d_mac.ether.ether_addr_octet[3] = 0x54;
  g_nt26.dev.netdev.d_mac.ether.ether_addr_octet[4] = 0x32;
  g_nt26.dev.netdev.d_mac.ether.ether_addr_octet[5] = 0x60;
  ret = netdev_lower_register(&g_nt26.dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      syslog(LOG_ERR, "NT26: netdev register failed: %d\n", ret);
      return ret;
    }
  return OK;
}

static int nt26_prepare_control(void)
{
  char response[768];
  int ret;
  int attempt;

  if (g_nt26.control_ready)
    {
      return OK;
    }
  if (!g_nt26_created)
    {
      memset(&g_nt26, 0, sizeof(g_nt26));
      g_nt26.uart_fd = -1;
      g_nt26.signal_strength = 99;
      g_nt26.lte_rsrp_dbm = -999;
      g_nt26.lte_rsrq_index = -1;
      g_nt26.sim_slot = -1;
      nxmutex_init(&g_nt26.tx_lock);
      nxmutex_init(&g_nt26.at_lock);
      nxmutex_init(&g_nt26.response_lock);
      nxmutex_init(&g_nt26.rx_lock);
      nxmutex_init(&g_nt26.tx_queue_lock);
      nxsem_init(&g_nt26.tx_ready, 0, 0);
      nxsem_init(&g_nt26.tx_ack, 0, 0);
      nxsem_init(&g_nt26.at_done, 0, 0);
      nxsem_init(&g_nt26.handshake_done, 0, 0);
      g_nt26_created = true;
    }

  /* TCA9555 P0.7 is an enable/reset line; the reference keeps it high. */
  ret = metalio_tca9555_write_pin(BOARD_IOEXP_RST_4G, true);
  if (ret < 0)
    {
      syslog(LOG_ERR, "NT26: module enable failed: %d\n", ret);
      return ret;
    }
  usleep(500000);

  if (g_nt26.uart_fd < 0)
    {
      ret = nt26_init_uart();
      if (ret < 0)
        {
          syslog(LOG_ERR, "NT26: UART open/config failed: %d\n", ret);
          return ret;
        }
    }

  /* Large scan replies belong in the heap, not scarce internal SRAM. */
  if (g_nt26.at_response == NULL)
    {
      g_nt26.at_response = kmm_zalloc(4096);
      if (g_nt26.at_response == NULL) return -ENOMEM;
      g_nt26.at_capacity = 4096;
    }
  /* Keep the 1604-byte frame off the AT/signal task stacks. */
  if (g_nt26.tx_buffer == NULL)
    {
      g_nt26.tx_buffer = kmm_malloc(NT26_MAX_FRAME);
      if (g_nt26.tx_buffer == NULL)
        {
          return -ENOMEM;
        }
    }
  if (g_nt26.rx_stream == NULL)
    {
      g_nt26.rx_stream = kmm_malloc(NT26_RX_STREAM_SIZE);
      if (g_nt26.rx_stream == NULL)
        {
          return -ENOMEM;
        }
    }

  /* A failed SIM/registration attempt preserves the AT transport. Retrying
   * must reuse it, never create duplicate readers or register eth2. */
  if (g_nt26.rx_task <= 0)
    {
      g_nt26.rx_task = task_create("nt26_rx", 120, 4096,
                                   nt26_rx_worker, NULL);
      if (g_nt26.rx_task < 0)
        {
          return -errno;
        }
    }
  if (g_nt26.dev.netdev.d_private == NULL)
    {
      ret = nt26_register_netdev();
      if (ret < 0)
        {
          return ret;
        }
    }
  if (g_nt26.tx_task <= 0)
    {
      /* TX must promptly service ACKs and keepalives even while the UART RX
       * task is draining modem bursts. Keep this priority local to NT26. */
      g_nt26.tx_task = task_create("nt26_tx", 110, 4096,
                                   nt26_tx_worker, NULL);
      if (g_nt26.tx_task < 0)
        {
          return -errno;
        }
    }

  /* Wake the modem and verify that the framed AT control channel responds. */
  ret = -ETIMEDOUT;
  for (attempt = 0; attempt < 5; attempt++)
    {
      ret = nt26_send_at("AT", response, sizeof(response), 1200);
      if (ret == OK)
        {
          break;
        }
      usleep(200000);
    }
  if (ret < 0)
    {
      /* Reference NT26 firmware also ships at 3 Mbaud. Probe it only
       * after the normal 2 Mbaud transport has failed validation. */
      struct termios tio;
      if (tcgetattr(g_nt26.uart_fd, &tio) == 0)
        {
          cfsetispeed(&tio, B3000000);
          cfsetospeed(&tio, B3000000);
          nxmutex_lock(&g_nt26.tx_lock);
          int changed = tcsetattr(g_nt26.uart_fd, TCSANOW, &tio);
          nxmutex_unlock(&g_nt26.tx_lock);
          if (changed == 0)
            {
              nxmutex_lock(&g_nt26.rx_lock);
              g_nt26.rx_length = 0;
              nxmutex_unlock(&g_nt26.rx_lock);
              for (attempt = 0; attempt < 4 && ret < 0; ++attempt)
                ret = nt26_send_at("AT", response, sizeof(response), 1200);
              if (ret == OK) syslog(LOG_INFO, "NT26: detected 3000000 baud\n");
              else
                {
                  cfsetispeed(&tio, NT26_BAUD);
                  cfsetospeed(&tio, NT26_BAUD);
                  nxmutex_lock(&g_nt26.tx_lock);
                  (void)tcsetattr(g_nt26.uart_fd, TCSANOW, &tio);
                  nxmutex_unlock(&g_nt26.tx_lock);
                }
            }
        }
    }
  if (ret < 0)
    {
      syslog(LOG_ERR, "NT26: no framed AT response (ret=%d) srdy=%d mrdy=%d tx=%lu rx=%lu bytes=%lu bad=%lu partial=%u\n",
             ret, esp_gpioread(BOARD_NT26_SRDY_GPIO),
             esp_gpioread(BOARD_NT26_MRDY_GPIO), g_nt26.tx_frames,
             g_nt26.rx_frames, g_nt26.rx_bytes, g_nt26.rx_invalid,
             (unsigned)g_nt26.rx_length);
      return ret;
    }

  ret = nt26_send_at("AT+ECSIMCFG?", response, sizeof(response), 2000);
  if (ret == OK)
    {
      char *slot = strstr(response, "\"SimSlot\"");
      int actual;
      if (slot != NULL && sscanf(slot, "\"SimSlot\",%d", &actual) == 1 &&
          (actual == 0 || actual == 1))
        {
          g_nt26.sim_slot = actual;
        }
    }

  nt26_confirm_caller_reporting();
  g_nt26.control_ready = true;
  return OK;
}

static bool nt26_pdp_active(const char *response)
{
  const char *p = response;
  while ((p = strstr(p, "+CGACT:")) != NULL)
    {
      int cid = -1, active = 0;
      if (sscanf(p, "+CGACT: %d,%d", &cid, &active) == 2 && cid == 1)
        return active == 1;
      p += 7;
    }
  return false;
}

static int nt26_initialize(void)
{
  if (!atomic_load(&g_radio_allowed)) return -ENETDOWN;
  static const uint8_t handshake[] =
    {0x53, 0x50, 0x49, 0x43, 0x02, 0x04, 0x0c, 0x00, 0, 0, 0, 0};
  char response[768];
  bool modem_reset = false;
  bool data_path_started = false;
  int attempt;
  int ret = nt26_prepare_control();
  if (ret < 0) return ret;
  ret = nt26_send_at("AT+CFUN=1", response, sizeof(response), 5000);
  if (ret < 0) return ret;

  for (attempt = 0; attempt < 6; attempt++)
    {
      if (nt26_check_sim_ready()) break;
      if (g_sim_probe_result == -EACCES) {
        if (nt26_try_remembered_pin()) break;
        return -EACCES;
      }
      if (attempt < 5) usleep(1000000);
    }
  if (attempt == 6) return g_sim_probe_result;
  // READY cards never receive a PIN write. Preserve an already unlocked
  // PIN-enabled SIM even after a host-only restart lost our RAM flag.
  const bool keep_sim_unlocked = nt26_preserve_unlocked_sim();

  struct metalio_sim_profile_s profile = {0};
  profile.slot = g_nt26.sim_slot;
  bool identity_known = nt26_read_sim_identity(profile.iccid) == OK;
  if (identity_known)
    (void)metalio_app_load_sim_profile(profile.iccid, &profile);
  /* Use a saved APN only for this ICCID. A new Mobile/Unicom/Telecom SIM
   * uses its subscription's automatic APN in either slot. Do not turn a
   * transient attach failure into a different card's private APN. */
  bool same_profile = identity_known && g_nt26.profile_applied &&
      profile.slot == g_nt26.profile.slot &&
      strcmp(profile.iccid, g_nt26.profile.iccid) == 0 &&
      strcmp(profile.apn, g_nt26.profile.apn) == 0 &&
      strcmp(profile.username, g_nt26.profile.username) == 0 &&
      strcmp(profile.password, g_nt26.profile.password) == 0 &&
      profile.auth == g_nt26.profile.auth;
  if (g_nt26.initialized && same_profile)
    {
      ret = nt26_send_at("AT+CEREG?", response, sizeof(response), 2500);
      if (ret == OK && (g_nt26.reg_status == 1 || g_nt26.reg_status == 5))
        {
          ret = nt26_send_at("AT+ECNETDEVCTL?", response, sizeof(response), 2500);
          char *field = strstr(response, "+ECNETDEVCTL:");
          int state = 0;
          if (ret == OK && field != NULL &&
              sscanf(field, "+ECNETDEVCTL: %*d,%*d,%*d,%d", &state) == 1 &&
              state == 1) return OK;
        }
    }
  g_nt26.initialized = false;
  /* The modem only serves DHCP on its UART Ethernet port after its NAT
   * configuration has been committed by a modem reset.  Match the reference
   * project's query/configure/reset sequence instead of silently assuming
   * the write-only AT commands succeeded. */
  ret = nt26_send_at("AT+ECNETCFG?", response, sizeof(response), 2000);
  if (ret < 0 || strstr(response, "+ECNETCFG: \"nat\",1") == NULL)
    {
      syslog(LOG_INFO, "NT26: configuring UART Ethernet NAT (query=%d)\n", ret);
      ret = nt26_send_at("AT+ECPCFG=\"usbCtrl\",1", response,
                         sizeof(response), 3000);
      if (ret < 0)
        {
          syslog(LOG_ERR, "NT26: usbCtrl configuration failed: %d (%s)\n",
                 ret, response);
          return ret;
        }
      ret = nt26_send_at("AT+ECNETCFG=\"nat\",1,\"192.168.10.2\"",
                         response, sizeof(response), 3000);
      if (ret < 0)
        {
          syslog(LOG_ERR, "NT26: NAT configuration failed: %d (%s)\n",
                 ret, response);
          return ret;
        }
      modem_reset = true;
    }

  if (!same_profile)
    {
      char command[320];
      if (keep_sim_unlocked)
        {
          /* CFUN=0 powers down the SIM and revokes a successful PIN unlock.
           * Reconfigure only its data context while keeping the SIM alive. */
          ret = nt26_send_at("AT+CGACT?", response, sizeof(response), 2500);
          if (ret == OK && nt26_pdp_active(response))
            ret = nt26_send_at("AT+CGACT=0,1", response, sizeof(response), 10000);
        }
      else ret = nt26_send_at("AT+CFUN=0", response, sizeof(response), 8000);
      if (ret < 0) return ret;
      snprintf(command, sizeof(command), "AT+CGDCONT=1,\"IP\",\"%s\"", profile.apn);
      ret = nt26_send_at(command, response, sizeof(response), 3000);
      if (ret == OK)
        {
          snprintf(command, sizeof(command), "AT+CGAUTH=1,%d,\"%s\",\"%s\"",
                   profile.auth, profile.username, profile.password);
          int auth_ret = nt26_send_at(command, response, sizeof(response), 3000);
          /* Older firmware may lack CGAUTH entirely. Automatic/no-auth
           * cards can still attach; explicitly requested auth must succeed. */
          if (auth_ret < 0 && (profile.auth != 0 || auth_ret != -EIO))
            ret = auth_ret;
        }
      int radio = keep_sim_unlocked ? OK :
          nt26_send_at("AT+CFUN=1", response, sizeof(response), 8000);
      if (ret < 0 || radio < 0)
        {
          g_nt26.profile_applied = false;
          return ret < 0 ? ret : radio;
        }
      nxmutex_lock(&g_nt26.response_lock);
      g_nt26.profile = profile;
      nxmutex_unlock(&g_nt26.response_lock);
      g_nt26.profile_applied = identity_known;
      syslog(LOG_INFO, "NT26: SIM profile slot=%d mode=%s applied\n",
             profile.slot, profile.apn[0] ? "custom" : "automatic");
    }

  if (modem_reset)
    {
      if (keep_sim_unlocked)
        {
          /* Never silently destroy a freshly verified PIN session. */
          syslog(LOG_ERR, "NT26: initial NAT setup requires reset before SIM unlock\n");
          return -EAGAIN;
        }
      ret = nt26_send_at("AT+ECRST", response, sizeof(response), 1000);
      /* Some NT26 firmware drops the UART as soon as ECRST is accepted, so
       * a missing final OK is expected.  Confirm the reset by polling AT. */
      if (ret < 0)
        {
          syslog(LOG_INFO, "NT26: ECRST ended UART transaction (%d); waiting for reboot\n",
                 ret);
        }
      usleep(1500000);
      for (attempt = 0; attempt < 20; attempt++)
        {
          ret = nt26_send_at("AT", response, sizeof(response), 1000);
          if (ret == OK)
            {
              break;
            }
          usleep(250000);
        }
      if (attempt == 20)
        {
          syslog(LOG_ERR, "NT26: no AT response after config reset\n");
          return -ETIMEDOUT;
        }
      ret = nt26_send_at("AT+CFUN=1", response, sizeof(response), 5000);
      if (ret < 0)
        {
          syslog(LOG_ERR, "NT26: full-function mode after reset failed: %d\n", ret);
          return ret;
        }
    }

  // Detect a re-locked SIM promptly instead of waiting for registration timeout.
  for (attempt = 0; attempt < 10; attempt++)
    {
      if (nt26_check_sim_ready()) break;
      if (g_sim_probe_result == -EACCES) {if(nt26_try_remembered_pin())break;return -EACCES;}
      usleep(500000);
    }
  if (attempt == 10) return g_sim_probe_result;

  // Discard an old SIM/RF session's registration before enabling reports,
  // not after: the command can immediately produce the new valid URC.
  nxmutex_lock(&g_nt26.response_lock);
  g_nt26.reg_status = 0;
  nxmutex_unlock(&g_nt26.response_lock);
  ret = nt26_send_at("AT+CEREG=2", response, sizeof(response), 2000);
  if (ret < 0)
    {
      syslog(LOG_ERR, "NT26: registration reporting setup failed: %d\n", ret);
      return ret;
    }
  clock_t registration_start = clock_systime_ticks();
  bool registered = false;
  for (attempt = 0; attempt < 60 &&
       TICK2MSEC(clock_systime_ticks() - registration_start) < 60000; attempt++)
    {
      ret = nt26_send_at("AT+CEREG?", response, sizeof(response), 2000);
      // A valid CEREG unsolicited report also confirms registration. A
      // delayed query OK must not overwrite that fact with a false timeout.
      if (metalio_nt26_is_registered())
        {
          registered = true;
          break;
        }
      usleep(1000000);
    }
  if (!registered)
    {
      syslog(LOG_ERR, "NT26: cellular registration timeout (CEREG=%d)\n",
             g_nt26.reg_status);
      return -ETIMEDOUT;
    }

  if (keep_sim_unlocked)
    {
      ret = nt26_send_at("AT+CGACT?", response, sizeof(response), 2500);
      if (ret < 0) return ret;
      if (!nt26_pdp_active(response))
        {
          int activate = nt26_send_at("AT+CGACT=1,1", response, sizeof(response), 20000);
          ret = nt26_send_at("AT+CGACT?", response, sizeof(response), 2500);
          if (ret < 0 || !nt26_pdp_active(response))
            return activate < 0 ? activate : -ENETUNREACH;
        }
    }

  ret = nt26_send_at("AT+ECNETDEVCTL?", response, sizeof(response), 2000);
  if (ret == OK)
    {
      int state = 0;
      char *state_field = strstr(response, "+ECNETDEVCTL:");
      if (state_field != NULL &&
          sscanf(state_field, "+ECNETDEVCTL: %*d,%*d,%*d,%d", &state) == 1)
        {
          data_path_started = state == 1;
        }
    }

  if (!data_path_started)
    {
      /* A handshake from an earlier SIM/session cannot acknowledge this
       * new Ethernet session. Drain the old semaphore before starting it. */
      g_nt26.handshake_seen = false;
      while (nxsem_trywait(&g_nt26.handshake_done) == OK) { }
      ret = nt26_send_at("AT+ECNETDEVCTL=2,1,1", response,
                         sizeof(response), 6000);
      if (ret < 0)
        {
          syslog(LOG_ERR, "NT26: Ethernet data path start failed: %d (%s)\n",
                 ret, response);
          return ret;
        }
    }
  else
    {
      /* The modem can retain the UART Ethernet session across a host restart.
       * Match the reference implementation: do not restart an already active
       * data path or send a second handshake into it. */
      g_nt26.handshake_seen = true;
      g_nt26.carrier = true;
      netdev_lower_carrier_on(&g_nt26.dev);
      syslog(LOG_INFO, "NT26: UART Ethernet data path already active\n");
    }

  if (!data_path_started)
    {
      ret = nt26_send_frame(handshake, sizeof(handshake), NT26_FRAME_ETH);
      if (ret < 0)
        {
          syslog(LOG_ERR, "NT26: handshake request failed: %d\n", ret);
          return ret;
        }
      ret = nxsem_tickwait(&g_nt26.handshake_done, MSEC2TICK(5000));
      if (ret < 0 || !g_nt26.handshake_seen)
        {
          syslog(LOG_ERR, "NT26: UART Ethernet handshake timeout\n");
          return -ETIMEDOUT;
        }
    }

  g_nt26.initialized = true;
  syslog(LOG_INFO, "NT26: 4G registered; cellular netdev ready\n");
  return OK;
}

int metalio_nt26_initialize(void)
{
  int ret;
  nxmutex_lock(&g_nt26_init_lock);
  ret = nt26_initialize();
  if (g_nt26.control_ready && g_nt26.signal_task <= 0)
    g_nt26.signal_task = task_create("nt26_sig", 110, 4096,
                                     nt26_signal_worker, NULL);
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

bool metalio_nt26_radio_enabled(void)
{
  return atomic_load(&g_radio_enabled);
}

int metalio_nt26_set_radio(bool enabled)
{
  char response[128] = {0};
  int mode = -1;
  nxmutex_lock(&g_nt26_init_lock);
  if (!enabled && g_nt26.control_ready && atomic_load(&g_call_state) != METALIO_CALL_IDLE)
    {
      (void)nt26_send_at("ATH", response, sizeof(response), 5000);
      nt26_update_call_state(METALIO_CALL_IDLE);
      (void)metalio_nt26_set_call_audio_path(false);
    }
  const bool previously_allowed = atomic_load(&g_radio_allowed);
  atomic_store(&g_radio_allowed, enabled);
  int ret = nt26_prepare_control();
  // CFUN=4 disables RF while retaining the SIM/PIN session, like airplane
  // mode. CFUN=0 also removes SIM power and would require the PIN again.
  int off_mode = !enabled && ret == OK && nt26_preserve_unlocked_sim() ? 4 : 0;
  bool already = false;
  if (ret == OK) {
    int query = nt26_send_at("AT+CFUN?", response, sizeof(response), 2500);
    char *field = strstr(response, "+CFUN:");
    already = query == OK && field && sscanf(field, "+CFUN: %d", &mode) == 1 &&
              mode == (enabled ? 1 : off_mode);
    if (already) atomic_store(&g_radio_enabled, enabled);
  }
  if (ret == OK && !already) {
    int command_ret = nt26_send_at(enabled ? "AT+CFUN=1" :
                                  off_mode == 4 ? "AT+CFUN=4" : "AT+CFUN=0",
                                  response, sizeof(response), 8000);
    // RF may change successfully before the command's final OK arrives.
    // Confirm by a read-only query even after timeout; never resend RF/PIN
    // writes simply because an acknowledgement was delayed.
    int query = -EIO;
    for (int attempt = 0; attempt < 2; ++attempt) {
      response[0] = '\0';
      query = nt26_send_at("AT+CFUN?", response, sizeof(response), 2500);
      char *field = strstr(response, "+CFUN:");
      if (query == OK && field && sscanf(field, "+CFUN: %d", &mode) == 1) break;
      if (attempt == 0) usleep(250000);
    }
    ret = query == OK && mode == (enabled ? 1 : off_mode) ? OK :
          command_ret < 0 ? command_ret : query < 0 ? query : -EIO;
    if (ret == OK) atomic_store(&g_radio_enabled, enabled);
  }
  if (ret < 0) atomic_store(&g_radio_allowed, previously_allowed);
  if (!enabled && ret == OK) {
    nt26_update_call_state(METALIO_CALL_IDLE);
    (void)metalio_nt26_set_call_audio_path(false);
    g_nt26.initialized = false;
    g_nt26.carrier = false;
    if (g_nt26.control_ready) netdev_lower_carrier_off(&g_nt26.dev);
    nxmutex_lock(&g_nt26.response_lock);
    g_nt26.reg_status = 0;
    g_nt26.signal_strength = 99;
    g_nt26.lte_rsrp_dbm = -999;
    nxmutex_unlock(&g_nt26.response_lock);
  }
  /* WiFi phone/SMS standby needs SIM and registration monitoring too.
   * Reuse the singleton worker already used by 4G data initialization. */
  if (enabled && ret == OK && g_nt26.control_ready && g_nt26.signal_task <= 0)
    g_nt26.signal_task = task_create("nt26_sig", 110, 4096,
                                     nt26_signal_worker, NULL);
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

/* CGCONTRDP fields 5 and 6 carry the DNS servers assigned by the
 * operator.  The LAN proxy advertised by NT26 DHCP can be unreachable;
 * use the actual PDP servers rather than guessing from the gateway. */
static unsigned int nt26_parse_dns(const char *response, uint32_t servers[2])
{
  const char *p = strstr(response, "+CGCONTRDP:");
  unsigned int count = 0;
  servers[0] = servers[1] = 0;
  if (p == NULL)
    {
      return 0;
    }
  p += strlen("+CGCONTRDP:");
  for (unsigned int field = 0; field <= 6; field++)
    {
      const char *begin = p;
      bool quoted = false;
      while (*p != '\0' && *p != '\r' && *p != '\n')
        {
          if (*p == '"') quoted = !quoted;
          if (*p == ',' && !quoted) break;
          p++;
        }
      const char *end = p;
      while (begin < end && (*begin == ' ' || *begin == '\t')) begin++;
      while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) end--;
      if (end - begin >= 2 && *begin == '"' && end[-1] == '"')
        {
          begin++;
          end--;
        }
      if (field >= 5 && end > begin && end - begin < INET_ADDRSTRLEN)
        {
          char address[INET_ADDRSTRLEN];
          struct in_addr parsed;
          memcpy(address, begin, end - begin);
          address[end - begin] = '\0';
          if (inet_pton(AF_INET, address, &parsed) == 1 && parsed.s_addr != 0)
            {
              servers[field - 5] = parsed.s_addr;
              count++;
            }
        }
      if (*p != ',') break;
      p++;
    }
  return count;
}

int metalio_nt26_get_dns_servers(uint32_t servers[2])
{
  char response[768];
  int ret;
  if (servers == NULL) return -EINVAL;
  servers[0] = servers[1] = 0;
  if (!g_nt26.control_ready) return -ENODEV;
  ret = nt26_send_at("AT+CGCONTRDP=1", response, sizeof(response), 3000);
  return ret < 0 ? ret : (int)nt26_parse_dns(response, servers);
}

int metalio_nt26_read_sim_profile(struct metalio_sim_profile_s *profile)
{
  if (!profile) return -EINVAL;
  nxmutex_lock(&g_nt26_init_lock);
  int ret = nt26_prepare_control();
  if (ret == OK)
    {
      ret = -ENODEV;
      for (int attempt = 0; attempt < 6; attempt++)
        {
          if (nt26_check_sim_ready()) { ret = OK; break; }
          if (g_sim_probe_result == -EACCES)
            {
              // Receive-only WiFi standby needs the same remembered unlock
              // as data initialization. This helper checks actual ICCID and
              // current SIM PIN state, and limits attempts to the saved card.
              // READY cards never reach this branch.
              if (nt26_try_remembered_pin()) { ret = OK; break; }
              ret = -EACCES;
              break;
            }
          usleep(500000);
        }
    }
  if (ret == OK)
    {
      memset(profile, 0, sizeof(*profile));
      profile->slot = g_nt26.sim_slot;
      ret = nt26_read_sim_identity(profile->iccid);
      if (ret == OK) (void)metalio_app_load_sim_profile(profile->iccid, profile);
      if (ret == OK && !g_nt26.profile_applied)
        {
          nxmutex_lock(&g_nt26.response_lock);
          strlcpy(g_nt26.profile.iccid, profile->iccid, sizeof(g_nt26.profile.iccid));
          g_nt26.profile.slot = profile->slot;
          nxmutex_unlock(&g_nt26.response_lock);
        }
    }
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

int metalio_nt26_get_sim_profile(struct metalio_sim_profile_s *profile)
{
  if (!profile) return -EINVAL;
  if (!g_nt26.control_ready) return -ENODEV;
  nxmutex_lock(&g_nt26.response_lock);
  // This getter is used by SMS. A valid SIM identity does not require an
  // applied Internet APN: WiFi-only telephony keeps that flag false.
  bool ready = g_nt26.sim_ready && g_nt26.profile.iccid[0] &&
               g_nt26.profile.slot == g_nt26.sim_slot;
  if (ready) *profile = g_nt26.profile;
  nxmutex_unlock(&g_nt26.response_lock);
  return ready ? OK : -ENODATA;
}

int metalio_nt26_unlock_sim(const char *pin, int expected_slot)
{
  size_t length = pin ? strnlen(pin, 9) : 0;
  if (length < 4 || length > 8 || (expected_slot != 0 && expected_slot != 1))
    return -EINVAL;
  for (size_t i = 0; i < length; i++)
    if (pin[i] < '0' || pin[i] > '9') return -EINVAL;
  char response[128] = {0};
  char command[32] = {0};
  nxmutex_lock(&g_nt26_init_lock);
  int ret = nt26_prepare_control();
  if (ret == OK && expected_slot != g_nt26.sim_slot) ret = -ESTALE;
  if (ret == OK && !atomic_load(&g_radio_allowed)) ret = -ENETDOWN;
  if (ret == OK && atomic_load(&g_call_state) != METALIO_CALL_IDLE) ret = -EBUSY;
  if (ret == OK)
    {
      ret = nt26_send_at("AT+CPIN?", response, sizeof(response), 2000);
      char *state = strstr(response, "+CPIN:");
      if (state) { state += 6; while (*state == ' ' || *state == '\t') state++; }
      if (ret == OK && state && strncmp(state, "SIM PIN", 7) == 0 &&
          (state[7] == '\r' || state[7] == '\n' || state[7] == '\0'))
        {
          snprintf(command, sizeof(command), "AT+CPIN=\"%s\"", pin);
          /* Exactly one attempt. Never guess, retry or submit a PUK as a PIN. */
          ret = nt26_send_at(command, response, sizeof(response), 5000);
          for (volatile char *p = command; p < command + sizeof(command); p++) *p = 0;
          for (volatile char *p = response; p < response + sizeof(response); p++) *p = 0;
          if (ret == OK)
            {
              ret = -EAGAIN;
              for (int i = 0; i < 10; i++)
                {
                  if (nt26_check_sim_ready()) { ret = OK; break; }
                  usleep(500000);
                }
              if (ret == OK) {
                nt26_invalidate_sim();
                g_nt26.keep_sim_unlocked = true;
              }
            }
          else {
            // A lost final OK must not turn an accepted PIN into a failure.
            if (nt26_check_sim_ready()) {
              nt26_invalidate_sim(); g_nt26.keep_sim_unlocked = true; ret = OK;
            } else ret = -EACCES;
          }
          if (ret == OK) {
            char id[METALIO_SIM_ID_MAX];
            if (nt26_read_sim_identity(id) == OK)
              (void)metalio_app_sim_pin_remember(id, pin);
          }
        }
      else if (ret == OK && state && strncmp(state, "READY", 5) == 0)
        { g_nt26.keep_sim_unlocked = true; ret = OK; }
      else if (ret == OK) ret = -EACCES;
    }
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

int metalio_nt26_get_sim_status(void)
{
  if (!g_nt26.control_ready) return METALIO_SIM_UNKNOWN;
  nxmutex_lock(&g_nt26.response_lock);
  int status = g_nt26.sim_status;
  if (g_nt26.sim_ready)
    status = g_nt26.reg_status == 3 ? METALIO_SIM_DENIED :
             (g_nt26.reg_status == 1 || g_nt26.reg_status == 5) ?
             METALIO_SIM_REGISTERED : METALIO_SIM_SEARCHING;
  nxmutex_unlock(&g_nt26.response_lock);
  return status;
}

static bool nt26_profile_field_valid(const char *text, size_t capacity)
{
  size_t n = strnlen(text, capacity);
  if (n == capacity) return false;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)text[i] < 32 || (unsigned char)text[i] > 126 ||
        text[i] == '\"' || text[i] == '\\') return false;
  return true;
}

int metalio_nt26_set_sim_profile(const struct metalio_sim_profile_s *profile)
{
  if (!profile || profile->auth < 0 || profile->auth > 3 ||
      !nt26_profile_field_valid(profile->apn, sizeof(profile->apn)) ||
      !nt26_profile_field_valid(profile->username, sizeof(profile->username)) ||
      !nt26_profile_field_valid(profile->password, sizeof(profile->password)) ||
      strnlen(profile->iccid, sizeof(profile->iccid)) >= sizeof(profile->iccid))
    return -EINVAL;
  nxmutex_lock(&g_nt26_init_lock);
  if (atomic_load(&g_call_state) != METALIO_CALL_IDLE)
    { nxmutex_unlock(&g_nt26_init_lock); return -EBUSY; }
  char current_id[METALIO_SIM_ID_MAX];
  int ret = nt26_check_sim_ready() ? nt26_read_sim_identity(current_id) : -ENODEV;
  if (ret == OK && (profile->slot != g_nt26.sim_slot ||
                   strcmp(current_id, profile->iccid) != 0)) ret = -ESTALE;
  if (ret == OK) ret = metalio_app_save_sim_profile(profile);
  if (ret == OK) nt26_invalidate_sim();
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

int metalio_nt26_get_sim_slot(void)
{
  return g_nt26.control_ready ? g_nt26.sim_slot : -1;
}

int metalio_nt26_is_registered(void)
{
  int registered;
  if (!g_nt26.control_ready) return 0;
  nxmutex_lock(&g_nt26.response_lock);
  registered = g_nt26.reg_status == 1 || g_nt26.reg_status == 5;
  nxmutex_unlock(&g_nt26.response_lock);
  return g_nt26.control_ready && registered;
}

int metalio_nt26_set_sim_slot(int slot)
{
  char command[40];
  char response[768];
  int ret;
  int previous;
  if (slot != 0 && slot != 1) return -EINVAL;
  nxmutex_lock(&g_nt26_init_lock);
  ret = nt26_prepare_control();
  if (ret < 0) goto out;
  if (atomic_load(&g_call_state) != METALIO_CALL_IDLE)
    { ret = -EBUSY; goto out; }
  previous = g_nt26.sim_slot;
  if (previous == slot) { ret = OK; goto out; }
  g_nt26.keep_sim_unlocked = false;
  nt26_invalidate_sim();
  nxmutex_lock(&g_nt26.response_lock);
  g_nt26.sim_ready = false;
  g_nt26.reg_status = 0;
  nxmutex_unlock(&g_nt26.response_lock);
  ret = nt26_send_at("AT+CFUN=0", response, sizeof(response), 8000);
  if (ret < 0) goto restore_radio;
  usleep(500000);
  snprintf(command, sizeof(command), "AT+ECSIMCFG=SimSlot,%d", slot);
  ret = nt26_send_at(command, response, sizeof(response), 5000);
  if (ret < 0) goto rollback_slot;
  usleep(500000);
  /* Read back the selected slot while RF is still off. CFUN=1 starts
   * registration and can delay/lose its reply on a SIM with no service;
   * that must not roll back an already confirmed SIM selection. */
  ret = -EIO;
  for (int attempt = 0; attempt < 4; ++attempt)
    {
      int query = nt26_send_at("AT+ECSIMCFG?", response, sizeof(response), 3000);
      char *field = query == OK ? strstr(response, "SimSlot") : NULL;
      int actual = -1;
      if (field != NULL)
        {
          field = strchr(field, ',');
          if (field != NULL && sscanf(field + 1, "%d", &actual) == 1 &&
              (actual == 0 || actual == 1))
            {
              g_nt26.sim_slot = actual;
              if (actual == slot) { ret = OK; break; }
            }
        }
      usleep(250000);
    }
  if (ret < 0) goto rollback_slot;

  g_nt26.carrier = false;
  netdev_lower_carrier_off(&g_nt26.dev);
  nxmutex_lock(&g_nt26.response_lock);
  g_nt26.signal_strength = 99;
  g_nt26.lte_rsrp_dbm = -999;
  g_nt26.reg_status = 0;
  nxmutex_unlock(&g_nt26.response_lock);
  /* The UI restarts after saving the confirmed slot; registration is
   * retried by network initialization even if this response times out. */
  if (atomic_load(&g_radio_allowed))
    {
      int radio = nt26_send_at("AT+CFUN=1", response, sizeof(response), 15000);
      syslog(LOG_INFO, "NT26: SIM slot=%d verified; radio restart=%d\n", slot, radio);
    }
  goto out;
rollback_slot:
  if (previous == 0 || previous == 1)
    {
      (void)nt26_send_at("AT+CFUN=0", response, sizeof(response), 8000);
      snprintf(command, sizeof(command), "AT+ECSIMCFG=SimSlot,%d", previous);
      int restore = nt26_send_at(command, response, sizeof(response), 5000);
      g_nt26.sim_slot = restore == OK ? previous : -1;
    }
restore_radio:
  // Even an AT timeout may have powered the radio down: always recover it.
  if (atomic_load(&g_radio_allowed))
    (void)nt26_send_at("AT+CFUN=1", response, sizeof(response), 15000);
out:
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

int metalio_nt26_get_signal_strength(void)
{
  int strength;
  if (!g_nt26.initialized)
    {
      return 99;
    }
  nxmutex_lock(&g_nt26.response_lock);
  strength = g_nt26.signal_strength;
  nxmutex_unlock(&g_nt26.response_lock);
  return strength;
}

int metalio_nt26_get_lte_rsrp_dbm(void)
{
  int rsrp;
  if (!g_nt26.initialized)
    {
      return -999;
    }
  nxmutex_lock(&g_nt26.response_lock);
  rsrp = g_nt26.lte_rsrp_dbm;
  nxmutex_unlock(&g_nt26.response_lock);
  return rsrp;
}

/* Read-only location/identity commands share the modem's AT transaction lock. */
int metalio_nt26_location_query(const char *command, char *response,
                                size_t response_size)
{
  if (command == NULL || response == NULL || response_size < 2)
    return -EINVAL;
  response[0] = '\0';
  if (strcmp(command, "AT+ECWIFISCAN") != 0 &&
      strcmp(command, "AT+ECBCINFO") != 0 && strcmp(command, "AT+CGSN") != 0)
    return -EINVAL;
  // Wi-Fi mode still needs NT26 identity/scanning for location. Prepare
  // the AT channel without selecting eth1 or changing the active route.
  nxmutex_lock(&g_nt26_init_lock);
  int ret = nt26_prepare_control();
  if (ret == OK) ret = nt26_send_at(command, response, response_size, 20000);
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

int metalio_nt26_call_dial(const char *number)
{
  char command[48];
  char response[768];
  size_t i;
  int ret;

  if (number == NULL || number[0] == '\0')
    {
      return -ENODEV;
    }

  /* Only pass a telephone number to the modem; reject AT-command injection. */
  for (i = 0; number[i] != '\0'; i++)
    {
      if (i >= 24 || ((number[i] < '0' || number[i] > '9') &&
                     !(i == 0 && number[i] == '+')))
        {
          return -EINVAL;
        }
    }
  if (number[0] == '+' && number[1] == '\0')
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_nt26_init_lock);
  ret = atomic_load(&g_call_state) == METALIO_CALL_IDLE ? nt26_prepare_control() : -EBUSY;
  if (ret == OK && !atomic_load(&g_radio_allowed)) ret = -ENETDOWN;
  /* Voice capability belongs to the active SIM subscription/modem, not
   * the physical slot number. Let either registered slot place the call. */
  if (ret == OK && !nt26_check_sim_ready() && !nt26_try_remembered_pin()) ret = -EACCES;
  // Voice service does not require an Internet APN, NAT or DHCP lease.
  if (ret == OK)
    {
      ret = -ENETUNREACH;
      for (int attempt = 0; attempt < 10; attempt++)
        {
          if (atomic_load(&g_call_release_requested)) { ret = -ECANCELED; break; }
          if (nt26_send_at("AT+CEREG?", response, sizeof(response), 2000) == OK &&
              (g_nt26.reg_status == 1 || g_nt26.reg_status == 5))
            { ret = OK; break; }
          usleep(1000000);
        }
    }
  if (ret == OK && atomic_load(&g_call_release_requested)) ret = -ECANCELED;
  if (ret == OK) ret = metalio_nt26_set_call_audio_path(true);
  if (ret == OK)
    {
      snprintf(command, sizeof(command), "ATD%s;", number);
      ret = nt26_send_at(command, response, sizeof(response), 10000);
      if (ret == OK && strstr(response, "OK") == NULL) ret = -EIO;
    }
  if (ret == OK && !atomic_load(&g_call_release_requested)) nt26_update_call_state(METALIO_CALL_DIALING);
  else (void)metalio_nt26_set_call_audio_path(false);
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

int metalio_nt26_call_answer(void)
{
  char response[256];
  nxmutex_lock(&g_nt26_init_lock);
  int ret = !g_nt26.control_ready ? -ENODEV :
            atomic_load(&g_call_state) != METALIO_CALL_INCOMING ? -ENOTCONN :
            atomic_load(&g_call_release_requested) ? -ECANCELED :
            metalio_nt26_set_call_audio_path(true);
  if (ret == OK) ret = nt26_send_at("ATA", response, sizeof(response), 10000);
  if (ret == OK && !atomic_load(&g_call_release_requested)) nt26_update_call_state(METALIO_CALL_ACTIVE);
  else (void)metalio_nt26_set_call_audio_path(false);
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

int metalio_nt26_call_hangup(void)
{
  char response[768];
  // Ending an existing call needs neither registration nor the data-network
  // lock. Never wait behind SIM selection or a Wi-Fi connection attempt.
  atomic_store(&g_call_release_requested, true);
  int ret = g_nt26.control_ready ? nt26_lock_call_urgent() : -ENODEV;
  if (ret == OK)
    {
      ret = nt26_send_at_locked("AT+CHUP", response, sizeof(response), 1800);
      if (ret < 0 && atomic_load(&g_call_state) != METALIO_CALL_IDLE)
        ret = nt26_send_at_locked("ATH", response, sizeof(response), 1800);
      if (ret == OK)
        {
          ret = -EAGAIN;
          for (int n = 0; n < 3; ++n)
            {
              int queried = nt26_send_at_locked("AT+CLCC", response, sizeof(response), 800);
              if (queried == OK && strstr(response, "OK") && !strstr(response, "+CLCC:"))
                { nt26_update_call_state(METALIO_CALL_IDLE); ret = OK; break; }
              if (atomic_load(&g_call_state) == METALIO_CALL_IDLE) { ret = OK; break; }
              usleep(100000);
            }
        }
      nxmutex_unlock(&g_nt26.at_lock);
    }
  if (atomic_load(&g_call_state) == METALIO_CALL_IDLE) ret = OK;
  if (atomic_load(&g_call_state) == METALIO_CALL_IDLE)
    (void)metalio_nt26_set_call_audio_path(false);
  return ret;
}

void metalio_nt26_call_request_hangup(void) { atomic_store(&g_call_release_requested, true); }
void metalio_nt26_call_begin_dial(void)
{
  atomic_store(&g_call_release_requested, false);
  /* An explicit outgoing call must not wait for a bulk inbox scan. Do
   * not interrupt a message already submitted to the carrier. */
  if (atomic_load(&g_sms_active) && !atomic_load(&g_sms_submitting))
    atomic_store(&g_sms_cancel, true);
}

int metalio_nt26_get_call_state(void) { return atomic_load(&g_call_state); }

int metalio_nt26_call_poll(void)
{
  char response[768];
  // Polling is best-effort: a call screen timer must not queue behind a long registration.
  if (nxmutex_trylock(&g_nt26_init_lock) < 0) return -EBUSY;
  if (g_nt26.control_ready && atomic_load(&g_call_state) == METALIO_CALL_IDLE &&
      (!g_clip_checked_once || clock_systime_ticks() - g_clip_checked >= MSEC2TICK(30000)))
    nt26_confirm_caller_reporting();
  int ret = g_nt26.control_ready ? nt26_send_at("AT+CLCC", response, sizeof(response), 2000) : -ENODEV;
  if (ret == OK)
    {
      int state = METALIO_CALL_IDLE;
      const char *p = response;
      while ((p = strstr(p, "+CLCC:")) != NULL)
        {
          int status = -1;
          if (sscanf(p, "+CLCC: %*d , %*d , %d", &status) == 1)
            {
              if (status == 0) { state = METALIO_CALL_ACTIVE; break; }
              if (status == 4 || status == 5) state = METALIO_CALL_INCOMING;
              else if (state != METALIO_CALL_INCOMING && (status == 2 || status == 3))
                state = METALIO_CALL_DIALING;
              else if (state == METALIO_CALL_IDLE && status == 1) state = METALIO_CALL_HELD;
            }
          p += 6;
        }
      nt26_update_call_state(state);
      if (state == METALIO_CALL_IDLE) (void)metalio_nt26_set_call_audio_path(false);
      ret = state;
    }
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

/* TS 27.007 CMUT/VTS are optional: update UI only after a positive ACK.
 * These commands share the existing modem lock and never restart RF/data. */
int metalio_nt26_call_control(int control, int value)
{
  char command[40], response[256] = {0};
  if (control < 1 || control > 5) return -EINVAL;
  if (control == 4 && (value < 0 || value > 100)) return -EINVAL;
  if (control == 2 && !((value >= '0' && value <= '9') || value == '*' || value == '#')) return -EINVAL;
  if ((control == 1 || control == 3) && value != 0 && value != 1) return -EINVAL;
  if (control == 3)
    {
      bool actual = false;
      int ret = metalio_tca9555_write_pin(BOARD_IOEXP_PA_ENABLE, value != 0);
      if (ret == OK) ret = metalio_tca9555_read_pin(BOARD_IOEXP_PA_ENABLE, &actual);
      return ret < 0 ? ret : actual == (value != 0) ? OK : -EIO;
    }
  nxmutex_lock(&g_nt26_init_lock);
  int state = atomic_load(&g_call_state);
  int ret = !g_nt26.control_ready ? -ENODEV : OK;
  if (ret == OK && (control == 4 || control == 5))
    {
      /* CLVL controls the NT26 analog voice output. The local I2S software
       * gain does not affect this path. Query after a write: an OK alone
       * must not make the UI display a volume the modem did not apply. */
      if (control == 4)
        {
          if (state != METALIO_CALL_ACTIVE && state != METALIO_CALL_HELD &&
              state != METALIO_CALL_DIALING) ret = -ENOTCONN;
          else
            {
              snprintf(command, sizeof(command), "AT+CLVL=%d", value);
              ret = nt26_send_at(command, response, sizeof(response), 2000);
              if (ret == OK && strstr(response, "OK") == NULL)
                ret = -EOPNOTSUPP;
            }
        }
      if (ret == OK)
        {
          ret = nt26_send_at("AT+CLVL?", response, sizeof(response), 2000);
          if (ret == OK)
            {
              const char *level = strstr(response, "+CLVL:");
              int actual = -1;
              if (strstr(response, "OK") == NULL || level == NULL ||
                  sscanf(level, "+CLVL: %d", &actual) != 1 ||
                  actual < 0 || actual > 100) ret = -EOPNOTSUPP;
              else
                {
                  ret = actual;
                  syslog(LOG_INFO, "NT26 call volume: requested=%d actual=%d\n",
                         control == 4 ? value : -1, actual);
                }
            }
        }
    }
  else if (ret == OK && control == 1 && (!value || state == METALIO_CALL_ACTIVE || state == METALIO_CALL_HELD))
    {
      // This operates on modem uplink voice, not the speaker or AI capture.
      // Read the real state first; unsupported firmware must never display a
      // successful mute based on a local UI boolean or an unrelated AT ACK.
      ret = nt26_send_at("AT+CMUT?", response, sizeof(response), 2000);
      int actual=-1;
      const char *field=strstr(response,"+CMUT:");
      if(ret==OK && (strstr(response,"OK")==NULL || !field ||
          sscanf(field,"+CMUT: %d",&actual)!=1 || (actual!=0 && actual!=1))) ret=-EOPNOTSUPP;
      if(ret==OK && actual!=value) {
        snprintf(command,sizeof(command),"AT+CMUT=%d",value);
        ret=nt26_send_at(command,response,sizeof(response),2000);
        if(ret==OK && strstr(response,"OK")==NULL) ret=-EOPNOTSUPP;
        if(ret==OK) {
          ret=nt26_send_at("AT+CMUT?",response,sizeof(response),2000);
          field=strstr(response,"+CMUT:");actual=-1;
          if(ret==OK && (strstr(response,"OK")==NULL || !field ||
              sscanf(field,"+CMUT: %d",&actual)!=1 || actual!=value)) ret=-EIO;
        }
      }
      if(ret==OK) syslog(LOG_INFO,"NT26 call microphone muted=%d verified\n",actual);
    }
  else if (ret == OK && control == 2 && state == METALIO_CALL_ACTIVE)
    {
      snprintf(command, sizeof(command), "AT+VTS=\"%c\"", value);
      ret = nt26_send_at(command, response, sizeof(response), 2000);
      if (ret == OK && strstr(response, "OK") == NULL) ret = -EOPNOTSUPP;
    }
  else if (ret == OK) ret = -ENOTCONN;
  nxmutex_unlock(&g_nt26_init_lock);
  return ret;
}

static int nt26_select_audio_locked(bool enabled)
{
  /* TCA9555 P0.1: 0 = NT26 voice, 1 = local playback. Change only
   * this bit, preserving the other Port 0 power/reset outputs. */
  bool actual = enabled;
  int ret = metalio_tca9555_write_pin(BOARD_IOEXP_PA_SWITCH, !enabled);
  if (ret == OK)
    ret = metalio_tca9555_read_pin(BOARD_IOEXP_PA_SWITCH, &actual);
  if (ret == OK && actual != !enabled) ret = -EIO;
  /* Retain ownership on a failed restore/readback. Local playback must
   * not steal a possibly live modem route after an I2C error. */
  if (ret == OK) g_call_audio_selected = enabled;
  else if (enabled) g_call_audio_selected = true;
  if (ret == OK && enabled) ret = metalio_nt26_call_control(3, 1);
  if (ret < 0)
    syslog(LOG_ERR, "NT26: audio route %s P0.1=%d failed %d\n",
           enabled ? "modem" : "local", !enabled, ret);
  return ret;
}

int metalio_nt26_set_call_audio_path(bool enabled)
{
  nxmutex_lock(&g_call_audio_lock);
  int ret = nt26_select_audio_locked(enabled);
  nxmutex_unlock(&g_call_audio_lock);
  return ret;
}

/* Change only the output sink during a real cellular call. P0.1 stays
 * on NT26 for both the analog Bluetooth bearer and the local speaker. */
int metalio_nt26_set_call_output(bool speaker)
{
  nxmutex_lock(&g_call_audio_lock);
  int state = atomic_load(&g_call_state);
  int ret = -ENOTCONN;
  if (state == METALIO_CALL_DIALING || state == METALIO_CALL_ACTIVE ||
      state == METALIO_CALL_HELD)
    {
      bool actual = true;
      ret = metalio_tca9555_write_pin(BOARD_IOEXP_PA_SWITCH, false);
      if (ret == OK) ret = metalio_tca9555_read_pin(BOARD_IOEXP_PA_SWITCH, &actual);
      if (ret == OK && actual) ret = -EIO;
      /* Preserve cellular ownership even if readback is uncertain. */
      g_call_audio_selected = true;
      if (ret == OK) ret = metalio_nt26_call_control(3, speaker ? 1 : 0);
    }
  nxmutex_unlock(&g_call_audio_lock);
  return ret;
}

int metalio_audio_select_local(void)
{
  nxmutex_lock(&g_call_audio_lock);
  int state = atomic_load(&g_call_state);
  int ret = g_call_audio_selected || state == METALIO_CALL_DIALING ||
            state == METALIO_CALL_ACTIVE || state == METALIO_CALL_HELD ?
            -EBUSY : nt26_select_audio_locked(false);
  nxmutex_unlock(&g_call_audio_lock);
  return ret;
}

#else
int metalio_nt26_set_call_output(bool speaker) { (void)speaker; return -ENOSYS; }
void metalio_nt26_call_request_hangup(void) { }
void metalio_nt26_call_begin_dial(void) { }
int metalio_nt26_call_control(int control,int value) { (void)control; (void)value; return -ENOSYS; }
int metalio_nt26_call_answer(void) { return -ENOSYS; }
int metalio_nt26_unlock_sim(const char *pin, int slot) { (void)pin; (void)slot; return -ENOSYS; }
int metalio_nt26_call_poll(void) { return -ENOSYS; }
int metalio_nt26_get_call_state(void) { return METALIO_CALL_IDLE; }
int metalio_nt26_read_sim_profile(struct metalio_sim_profile_s *p)
{ (void)p; return -ENOSYS; }
int metalio_nt26_get_sim_profile(struct metalio_sim_profile_s *p)
{ (void)p; return -ENOSYS; }
int metalio_nt26_set_sim_profile(const struct metalio_sim_profile_s *p)
{ (void)p; return -ENOSYS; }
int metalio_nt26_get_sim_status(void) { return METALIO_SIM_UNKNOWN; }

bool metalio_nt26_radio_enabled(void) { return false; }

int metalio_nt26_set_radio(bool enabled)
{
  return enabled ? -ENOSYS : 0;
}

int metalio_nt26_initialize(void)
{
  return -ENOSYS;
}

int metalio_nt26_get_dns_servers(uint32_t servers[2])
{
  if (servers != NULL) servers[0] = servers[1] = 0;
  return -ENOSYS;
}

int metalio_nt26_get_sim_slot(void)
{
  return -ENOSYS;
}

int metalio_nt26_is_registered(void)
{
  return 0;
}

int metalio_nt26_set_sim_slot(int slot)
{
  (void)slot;
  return -ENOSYS;
}

int metalio_nt26_get_signal_strength(void)
{
  return 99;
}

int metalio_nt26_get_lte_rsrp_dbm(void)
{
  return -999;
}

int metalio_nt26_location_query(const char *command, char *response,
                                size_t response_size)
{
  (void)command;
  if (response != NULL && response_size > 0) response[0] = '\0';
  return -ENOSYS;
}

int metalio_nt26_call_dial(const char *number)
{
  (void)number;
  return -ENOSYS;
}

int metalio_nt26_call_hangup(void)
{
  return -ENOSYS;
}

int metalio_nt26_set_call_audio_path(bool enabled)
{
  (void)enabled;
  return -ENOSYS;
}

int metalio_audio_select_local(void)
{
  return -ENOSYS;
}

#endif /* CONFIG_METALIO_NT26_4G */

#ifdef CONFIG_METALIO_NT26_4G

/* SMS uses the same serialized AT channel as networking/call control. No
 * modem reset, SIM reconfiguration or automatic retry of a submitted SMS. */
static int nt26_sms_lock(void)
{
  if (!g_nt26.control_ready || !g_nt26.sim_ready ||
      !atomic_load(&g_radio_enabled)) return -ENETDOWN;
  if (atomic_load(&g_call_state) != METALIO_CALL_IDLE) return -EBUSY;
  /* The former 750 ms wait was shorter than a single 2500 ms telemetry
   * query. Queue behind that owner and yield subsequent telemetry cycles;
   * never wait indefinitely or hold the lock while waiting for a call to end. */
  atomic_store(&g_sms_waiting, true);
  int locked = nxmutex_timedlock(&g_nt26.at_lock, 5000);
  atomic_store(&g_sms_waiting, false);
  if (locked < 0)
    {
      syslog(LOG_WARNING,"NT26: SMS AT owner busy after bounded wait\n");
      return -EAGAIN;
    }
  if (!atomic_load(&g_radio_enabled) ||
      atomic_load(&g_call_state) != METALIO_CALL_IDLE)
    {
      nxmutex_unlock(&g_nt26.at_lock);
      return -EBUSY;
    }
  atomic_store(&g_sms_cancel, false);
  atomic_store(&g_sms_active, true);
  return OK;
}
static void nt26_sms_unlock(void)
{
  nxmutex_lock(&g_nt26.response_lock);
  g_nt26.sms_prompt = false;
  atomic_store(&g_sms_submitting, false);
  nxmutex_unlock(&g_nt26.response_lock);
  atomic_store(&g_sms_active, false);
  nxmutex_unlock(&g_nt26.at_lock);
}
unsigned int metalio_nt26_sms_revision(void) { return atomic_load(&g_sms_revision); }

/* SMS inbox queries own a large buffer through the actual response and its
 * terminal result. The UART bridge's early dispatch OK is not query data.
 * This is confined to receive queries; network/calls/SMS submission are intact. */
static int nt26_sms_query_locked(const char *command,const char *prefix,
                                char *out,size_t capacity,unsigned timeout_ms)
{
  char wire[96];int n=snprintf(wire,sizeof(wire),"%s\r",command);
  if(n<=0||(size_t)n>=sizeof(wire))return -E2BIG;
  while(nxsem_trywait(&g_nt26.at_done)==OK){}
  nxmutex_lock(&g_nt26.response_lock);
  char *previous=g_nt26.at_response;size_t previous_capacity=g_nt26.at_capacity;
  g_nt26.at_response=out;g_nt26.at_capacity=capacity;out[0]='\0';
  g_nt26.at_overflow=false;g_nt26.waiting_at=true;
  nxmutex_unlock(&g_nt26.response_lock);
  int ret=nt26_send_frame((const uint8_t*)wire,(size_t)n,NT26_FRAME_AT);
  clock_t started=clock_systime_ticks();
  if(ret==OK)for(;;){
    (void)nxsem_tickwait(&g_nt26.at_done,MSEC2TICK(100));
    nxmutex_lock(&g_nt26.response_lock);
    const char *actual=strstr(out,prefix);
    bool done=actual&&nt26_sms_response_complete(actual);
    bool failed=strstr(out,"ERROR")!=NULL,overflow=g_nt26.at_overflow;
    nxmutex_unlock(&g_nt26.response_lock);
    if(overflow){ret=-EOVERFLOW;break;}
    if(failed){ret=-EIO;break;}
    if(done)break;
    if(atomic_load(&g_sms_cancel)||atomic_load(&g_call_state)!=METALIO_CALL_IDLE||!atomic_load(&g_radio_enabled)||!atomic_load(&g_radio_allowed)){ret=-EAGAIN;break;}
    if(clock_systime_ticks()-started>=MSEC2TICK(timeout_ms)){ret=-ETIMEDOUT;break;}
  }
  nxmutex_lock(&g_nt26.response_lock);
  g_nt26.waiting_at=false;g_nt26.at_response=previous;g_nt26.at_capacity=previous_capacity;
  nxmutex_unlock(&g_nt26.response_lock);return ret;
}
/* Count complete PDU records and require their terminal result. Accept CRLF,
 * LF-only and an exact trailing OK, including a result split across frames. */
static unsigned nt26_sms_inbox_records(const char *text,bool *terminal)
{
  bool header=false;unsigned records=0;*terminal=false;
  for(const char *line=text;*line;){
    while(*line=='\r'||*line=='\n')++line;
    const char *end=strpbrk(line,"\r\n");size_t n=end?(size_t)(end-line):strlen(line);
    while(n&&(*line==' '||*line=='\t')){++line;--n;}
    while(n&&(line[n-1]==' '||line[n-1]=='\t'))--n;
    if(n>=6&&!strncmp(line,"+CMGL:",6)){header=true;*terminal=false;}
    else if(header&&n>=6&&!(n&1)&&end){
      size_t hex=0;while(hex<n&&isxdigit((unsigned char)line[hex]))++hex;
      if(hex==n){++records;header=false;}
    }else if(n==2&&!strncmp(line,"OK",2)&&!header&&records)*terminal=true;
    if(!end)break;line=end+1;
  }return records;
}
static int nt26_sms_read_inbox_locked(char *out,int expected)
{
  while(nxsem_trywait(&g_nt26.at_done)==OK){}
  nxmutex_lock(&g_nt26.response_lock);
  out[0]='\0';g_nt26.at_overflow=false;g_nt26.waiting_at=true;
  nxmutex_unlock(&g_nt26.response_lock);
  const char command[]="AT+CMGL=4\r";
  int ret=nt26_send_frame((const uint8_t*)command,sizeof(command)-1,NT26_FRAME_AT);
  clock_t started=clock_systime_ticks(),changed=started;size_t previous_size=0;
  unsigned records=0;bool terminal=false;
  if(ret==OK)for(;;){
    (void)nxsem_tickwait(&g_nt26.at_done,MSEC2TICK(100));clock_t now=clock_systime_ticks();
    nxmutex_lock(&g_nt26.response_lock);
    size_t size=strlen(out);if(size!=previous_size){changed=now;previous_size=size;}
    bool overflow=g_nt26.at_overflow,failed=strstr(out,"ERROR")!=NULL;
    records=nt26_sms_inbox_records(out,&terminal);
    bool empty=!records&&nt26_sms_response_complete(out)&&((expected==0&&now-changed>=MSEC2TICK(350))||(expected<0&&now-started>=MSEC2TICK(5000)));
    nxmutex_unlock(&g_nt26.response_lock);
    if(overflow){ret=-EOVERFLOW;break;}
    if(failed){ret=-EIO;break;}
    if((terminal&&(expected<0||records>=(unsigned)expected))||empty)break;
    if(atomic_load(&g_sms_cancel)||atomic_load(&g_call_state)!=METALIO_CALL_IDLE||!atomic_load(&g_radio_enabled)||!atomic_load(&g_radio_allowed)){ret=-EAGAIN;break;}
    if(now-started>=MSEC2TICK(5000)){ret=-ETIMEDOUT;break;}
  }
  nxmutex_lock(&g_nt26.response_lock);g_nt26.waiting_at=false;nxmutex_unlock(&g_nt26.response_lock);
  if(ret<0)syslog(LOG_WARNING,"NT26: SMS inbox response bytes=%lu records=%u terminal=%d result=%d cancel=%d radio=%d/%d call=%d\n",(unsigned long)previous_size,records,terminal,ret,atomic_load(&g_sms_cancel),atomic_load(&g_radio_enabled),atomic_load(&g_radio_allowed),atomic_load(&g_call_state));
  return ret;
}

/* NT26's UART bridge can drop an entire nonempty SIM CMGL response.
 * Read one SIM record at a time with bounded replies. Keep the same serialized
 * AT owner and synthesize only validated PDU lines for the existing importer.
 * No delete, send, SMS-center change or modem reset is performed. */
static int nt26_sms_read_sim_records_locked(char *out,size_t capacity,int expected,int slots)
{
  if(capacity<16384)return -ENOSPC;
  const size_t reply_capacity=8192,output_capacity=capacity-reply_capacity;
  char *reply=out+output_capacity;size_t have=0;unsigned found=0;out[0]='\0';
  if(expected==0)return OK;
  if(slots<=0||slots>256)slots=50;
  for(int index=0;index<=slots;++index){
    if(atomic_load(&g_sms_cancel)||!atomic_load(&g_radio_allowed)||
       !atomic_load(&g_radio_enabled)||atomic_load(&g_call_state)!=METALIO_CALL_IDLE)return -EAGAIN;
    char command[40];snprintf(command,sizeof(command),"AT+CMGR=%d",index);
    int ret=nt26_sms_query_locked(command,"+CMGR:",reply,reply_capacity,3000);
    if(ret<0){const char *error=strstr(reply,"+CMS ERROR:");int code=error?atoi(error+11):-1;
      if(ret==-EIO&&(code==321||code==322))continue;
      syslog(LOG_WARNING,"NT26: SMS SIM record index=%d result=%d cms=%d bytes=%lu\n",index,ret,code,(unsigned long)strlen(reply));
      return ret;
    }
    const char *header=strstr(reply,"+CMGR:");const char *pdu=header?strchr(header,'\n'):NULL;
    if(!pdu)return -EBADMSG;
    ++pdu;while(*pdu=='\r'||*pdu=='\n'||*pdu==' '||*pdu=='\t')++pdu;
    size_t bytes=strspn(pdu,"0123456789ABCDEFabcdef");
    if(bytes<6||(bytes&1)||(* (pdu+bytes)!='\r'&&*(pdu+bytes)!='\n'))return -EBADMSG;
    char prefix[48];int n=snprintf(prefix,sizeof(prefix),"+CMGL: %d,0,,0\r\n",index);
    if(n<=0||have+(size_t)n+bytes+3>output_capacity)return -EOVERFLOW;
    memcpy(out+have,prefix,n);have+=n;memcpy(out+have,pdu,bytes);have+=bytes;
    out[have++]='\r';out[have++]='\n';out[have]='\0';++found;
    if(expected>0&&found>=(unsigned)expected)break;
  }
  if(expected>=0&&found<(unsigned)expected)return -EAGAIN;
  return OK;
}

int metalio_nt26_sms_list(const char *storage, char *out, size_t capacity)
{
  if (!storage || (strcmp(storage,"SM") && strcmp(storage,"ME")) ||
      !out || capacity < 4096) return -EINVAL;
  int ret=nt26_sms_lock(); if(ret<0)return ret;
  char command[64],response[256];int expected=-1,slots=0;
  ret=nt26_send_at_locked("AT+CMGF=0",response,sizeof(response),2000);
  if(ret==OK && !atomic_load(&g_sms_cancel)) {
    ret=nt26_sms_query_locked("AT+CMGF?","+CMGF:",out,capacity,2500);
    if(ret==OK){const char *format=strstr(out,"+CMGF:");int value=-1;
      if(!format || sscanf(format,"+CMGF: %d",&value)!=1 || value!=0)ret=-EAGAIN;}
  }
  if(ret==OK && !atomic_load(&g_sms_cancel))
    ret=nt26_send_at_locked("AT+CNMI=2,1,0,0,0",response,sizeof(response),2000);
  if(ret==OK && !atomic_load(&g_sms_cancel)) {
    snprintf(command,sizeof(command),"AT+CPMS=\"%s\"",storage);
    ret=nt26_sms_query_locked(command,"+CPMS:",out,capacity,3000);
    if(ret==OK){const char *counts=strstr(out,"+CPMS:");int used=0,total=0;
      if(counts && sscanf(counts,"+CPMS: %d,%d",&used,&total)==2 && used>=0 && total>=used){expected=used;slots=total;}}
    if(ret==OK && !strcmp(storage,"SM") && expected>=0) {
      const char *field=strstr(out,"+CPMS:");int used=0,total=0;
      if(field && sscanf(field,"+CPMS: %d,%d",&used,&total)==2 && total>0) {
        snprintf(command,sizeof(command),"AT+CPMS=\"SM\",\"ME\",\"%s\"",used<total?"SM":"ME");
        ret=nt26_sms_query_locked(command,"+CPMS:",out,capacity,3000);
      }
    }
  }
  if(ret==OK && !atomic_load(&g_sms_cancel) && !strcmp(storage,"SM")) {
    ret=nt26_sms_read_sim_records_locked(out,capacity,expected,slots);
  } else if(ret==OK && !atomic_load(&g_sms_cancel)) {
    nxmutex_lock(&g_nt26.response_lock);
    char *previous=g_nt26.at_response;size_t previous_capacity=g_nt26.at_capacity;
    g_nt26.at_response=out;g_nt26.at_capacity=capacity;out[0]='\0';
    nxmutex_unlock(&g_nt26.response_lock);
    /* NT26 rejects the optional ,1 argument. Unread state is maintained
     * durably by HiOS, independently of the modem's read flag. */
    ret=nt26_sms_read_inbox_locked(out,expected);
    nxmutex_lock(&g_nt26.response_lock);
    g_nt26.at_response=previous;g_nt26.at_capacity=previous_capacity;
    nxmutex_unlock(&g_nt26.response_lock);
  }
  if(atomic_load(&g_sms_cancel) && ret==OK)ret=-EAGAIN;
  if(ret<0)syslog(LOG_WARNING,"NT26: SMS inbox bank=%s expected=%d result=%d\n",storage,expected,ret);
  nt26_sms_unlock();return ret;
}
static int nt26_sms_cms_code(const char *response)
{
  const char *p = strstr(response, "+CMS ERROR:");
  if (!p) return -1;
  p += 11;
  char *end;
  long code = strtol(p, &end, 10);
  return end != p && code >= 0 && code <= 65535 ? (int)code : -1;
}

static int nt26_sms_hex_digit(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int nt26_sms_hex_byte(const char *p)
{
  int a = nt26_sms_hex_digit(p[0]), b = nt26_sms_hex_digit(p[1]);
  return a < 0 || b < 0 ? -1 : a * 16 + b;
}

/* Obtain the SCA from the currently selected SIM on every send. No hardcoded
 * carrier/province numbers and no cached value that can cross a SIM swap.
 * EF_SMSP is read-only; never change the SIM's SMS parameters or AT charset.
 * Output includes the SCA length octet, ready for the outgoing PDU. */
static int nt26_sms_center_locked(char *sca, size_t capacity)
{
  char response[640];
  if (capacity < 26) return -EINVAL;
  int ret = nt26_send_at_locked("AT+CRSM=178,28482,1,4,0",
                                response, sizeof(response), 2500);
  const char *field = ret == OK ? strstr(response, "+CRSM:") : NULL;
  unsigned sw1 = 0, sw2 = 0;
  if (field && sscanf(field, "+CRSM: %u,%u", &sw1, &sw2) == 2 &&
      sw1 == 108 && sw2 >= 28 && sw2 <= 255 &&
      !atomic_load(&g_sms_cancel)) {
    char command[48];
    snprintf(command, sizeof(command), "AT+CRSM=178,28482,1,4,%u", sw2);
    ret = nt26_send_at_locked(command, response, sizeof(response), 2500);
    field = ret == OK ? strstr(response, "+CRSM:") : NULL;
  }
  if (field && sscanf(field, "+CRSM: %u,%u", &sw1, &sw2) == 2 &&
      ((sw1 == 144 && sw2 == 0) || sw1 == 145)) {
    const char *p = strchr(field, '"');
    const char *end = p ? strchr(++p, '"') : NULL;
    size_t n = end ? (size_t)(end - p) : 0;
    /* TS 31.102: alpha identifier followed by the 28-byte SMS parameters;
     * SCA occupies 12 bytes, beginning 15 bytes from the record's end. */
    if (n >= 56 && n <= 510 && !(n & 1)) {
      int pi = nt26_sms_hex_byte(p + n - 56);
      const char *a = p + n - 30;
      int bytes = nt26_sms_hex_byte(a), toa = nt26_sms_hex_byte(a + 2);
      bool valid = pi >= 0 && !(pi & 2) && bytes >= 2 && bytes <= 11 &&
                   (toa == 129 || toa == 145);
      for (int i = 0; valid && i < bytes - 1; ++i) {
        int b = nt26_sms_hex_byte(a + 4 + i * 2);
        valid = b >= 0 && (b & 15) <= 9 &&
                ((b >> 4) <= 9 || (i == bytes - 2 && (b >> 4) == 15));
      }
      if (valid) {
        size_t chars = (size_t)(bytes + 1) * 2;
        memcpy(sca, a, chars); sca[chars] = '\0';
        return OK;
      }
    }
  }
  if (atomic_load(&g_sms_cancel)) return -EAGAIN;
  /* Some cards do not expose EF_SMSP through restricted SIM access. Use the
   * module's standard CSCA query, including its UCS2 address representation. */
  ret = nt26_send_at_locked("AT+CSCA?", response, sizeof(response), 2000);
  field = ret == OK ? strstr(response, "+CSCA:") : NULL;
  char raw[96] = {0}, number[24] = {0}; unsigned toa = 0;
  if (!field || sscanf(field, "+CSCA: \"%95[^\"]\",%u", raw, &toa) != 2 ||
      (toa != 129 && toa != 145)) return -ENODATA;
  size_t length = strlen(raw);
  bool ucs2 = length >= 8 && !(length % 4) && raw[0] == '0' && raw[1] == '0';
  if (ucs2) {
    if (length / 4 >= sizeof(number)) return -ENODATA;
    for (size_t i = 0; i < length / 4; ++i) {
      if (raw[i * 4] != '0' || raw[i * 4 + 1] != '0') return -ENODATA;
      int c = nt26_sms_hex_byte(raw + i * 4 + 2);
      if (c < 0) return -ENODATA;
      number[i] = (char)c;
    }
  } else {
    if (length >= sizeof(number)) return -ENODATA;
    memcpy(number, raw, length + 1);
  }
  const char *digits = number[0] == '+' ? number + 1 : number;
  size_t count = strlen(digits);
  if (!count || count > 20) return -ENODATA;
  for (size_t i = 0; i < count; ++i)
    if (digits[i] < '0' || digits[i] > '9') return -ENODATA;
  if (number[0] == '+') toa = 145;
  snprintf(sca, capacity, "%02X%02X", (unsigned)(1 + (count + 1) / 2), toa);
  size_t pos = 4;
  for (size_t i = 0; i < count; i += 2) {
    sca[pos++] = i + 1 < count ? digits[i + 1] : 'F';
    sca[pos++] = digits[i];
  }
  sca[pos] = '\0';
  return OK;
}

int metalio_nt26_sms_send_pdu_ex(const char *hex, size_t tpdu_bytes,
                                int *cms_error)
{
  if(cms_error)*cms_error=-1;
  if(!hex)return -EINVAL;
  size_t len=strlen(hex);
  if(len<4 || len>1024 || (len&1) || tpdu_bytes!=len/2-1 ||
     hex[0]!='0' || hex[1]!='0')return -EINVAL;
  for(size_t i=0;i<len;++i)if(!isxdigit((unsigned char)hex[i]))return -EINVAL;
  int ret=nt26_sms_lock();if(ret<0)return ret;
  char response[512]={0},center[26];
  const char *stage="registration";
  /* RF may have just been restored while WiFi remains the Internet route.
   * Wait for registration BEFORE any submission; these are read-only
   * queries, never retries of an already submitted SMS. Yield to calls or
   * airplane mode, and do not require an APN, NAT or eth1 DHCP lease. */
  ret=-ENETUNREACH;
  clock_t registered_deadline=clock_systime_ticks()+MSEC2TICK(12000);
  for(unsigned attempt=0;attempt<8;++attempt) {
    if(atomic_load(&g_sms_cancel)||!atomic_load(&g_radio_allowed)||
       !atomic_load(&g_radio_enabled)||atomic_load(&g_call_state)!=METALIO_CALL_IDLE) {
      ret=-EAGAIN;break;
    }
    (void)nt26_send_at_locked("AT+CEREG?",response,sizeof(response),1500);
    if(metalio_nt26_is_registered()){ret=OK;break;}
    if(clock_systime_ticks()>=registered_deadline)break;
    usleep(500000);
  }
  if(ret==OK){stage="sms-center";ret=nt26_sms_center_locked(center,sizeof(center));}
  if(ret==OK && !atomic_load(&g_sms_cancel)) {
    stage="format";
    ret=nt26_send_at_locked("AT+CMGF=0",response,sizeof(response),2000);
  }
  if(ret==OK && !atomic_load(&g_sms_cancel) && atomic_load(&g_call_state)==METALIO_CALL_IDLE) {
    /* NT26's bridge does not provide a reliable interactive CMGS/ESC
     * session. An abandoned input context blocks CMGS, CMSS and ECSMSSEND
     * globally (CMS 300/303), even after a HiOS restart or RF cycle.
     * Submit the complete validated PDU in one AT transaction. This native
     * EC command has been accepted repeatedly, including concatenated UCS2.
     * Do not probe CMGS or send ESC/Ctrl-Z on this path. */
    stage="inline-submit";
    char body[1080];
    int body_len=snprintf(body,sizeof(body),"AT+ECSMSSEND=0,\"%s%s\"\r",center,hex+2);
    if(body_len<=0 || (size_t)body_len>=sizeof(body)) {
      nt26_sms_unlock();return -E2BIG;
    }
    while(nxsem_trywait(&g_nt26.at_done)==OK){}
    nxmutex_lock(&g_nt26.response_lock);
    g_nt26.at_response[0]='\0';g_nt26.at_overflow=false;g_nt26.waiting_at=true;
    atomic_store(&g_sms_submitting,true);
    nxmutex_unlock(&g_nt26.response_lock);
    ret=nt26_send_frame((const uint8_t*)body,(size_t)body_len,NT26_FRAME_AT);
    bool submitted=ret==OK,complete=false;
    if(submitted) {
      for(unsigned elapsed=0;elapsed<60000;elapsed+=100) {
        if(nxsem_tickwait(&g_nt26.at_done,MSEC2TICK(100))==OK){complete=true;break;}
        if(atomic_load(&g_sms_cancel) || atomic_load(&g_call_state)!=METALIO_CALL_IDLE ||
           !atomic_load(&g_radio_allowed) || !g_nt26.sim_ready)break;
      }
    }
    nxmutex_lock(&g_nt26.response_lock);
    g_nt26.waiting_at=false;
    bool rejected=strstr(g_nt26.at_response,"ERROR")!=NULL;
    bool accepted=complete && !rejected && nt26_sms_response_complete(g_nt26.at_response);
    atomic_store(&g_sms_submitting,false);
    if(cms_error)*cms_error=nt26_sms_cms_code(g_nt26.at_response);
    nxmutex_unlock(&g_nt26.response_lock);
    // Once the complete command may have left UART, a missing confirmation
    // is UNKNOWN. Never automatically resend or reset the modem here.
    if(submitted)ret=complete&&accepted?OK:rejected?-EIO:-EINPROGRESS;
    else ret=-EINPROGRESS; /* Some bytes may have left UART before an error. */
  } else {
    if(cms_error)*cms_error=nt26_sms_cms_code(response);
    if(ret==OK)ret=-EAGAIN;
  }
  if(ret<0)syslog(LOG_WARNING,"NT26: SMS stage=%s result=%d cms=%d\n",
                 stage,ret,cms_error?*cms_error:-1);
  nt26_sms_unlock();return ret;
}

int metalio_nt26_sms_send_pdu(const char *hex, size_t tpdu_bytes)
{
  return metalio_nt26_sms_send_pdu_ex(hex,tpdu_bytes,NULL);
}

#endif /* CONFIG_METALIO_NT26_4G */
