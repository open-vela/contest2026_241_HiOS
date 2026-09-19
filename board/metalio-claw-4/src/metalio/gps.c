/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/metalio/gps.c
 *
 * GPS UART bring-up + NMEA reader.
 *
 * The GNSS module is powered from the TCA9555 IO expander (P0.0,
 * high-enable) and streams NMEA sentences over UART0 (GPIO 38 = TX,
 * GPIO 37 = RX) at 9600 8N1.  UART0 is registered as /dev/ttyS0 because
 * the console is the USB CDC ACM serial device, not a hardware UART.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>

#include <arch/board/board.h>
#include <metalio/metalio.h>

#define GPS_UART_DEVICE "/dev/ttyS0"

static int g_gps_fd = -1;
static pthread_t g_gps_thread;

/* Parsed fix state, refreshed from each GGA sentence. */
typedef struct
{
  int fix_quality;   /* 0 = no fix, 1 = GPS, 2 = DGPS */
  int sats;          /* satellites used (GGA field 7) */
  bool pos_valid;
  double lat_deg;
  double lon_deg;
} gps_fix_t;

static gps_fix_t g_fix;
static int g_gga_count;

/****************************************************************************
 * Name: gps_nth_field
 *
 * Copy the idx-th comma-separated NMEA field of `line` into `out`.
 ****************************************************************************/

static int gps_nth_field(char *line, int idx, char *out, int outsz)
{
  char *p = line;
  int field = 0;
  int n = 0;

  if (p != NULL && *p == '$')
    {
      p++;
    }

  while (field < idx && *p != '\0')
    {
      while (*p != '\0' && *p != ',')
        {
          p++;
        }

      if (*p == ',')
        {
          p++;
        }

      field++;
    }

  while (*p != '\0' && *p != ',' && *p != '*' &&
         *p != '\r' && *p != '\n' && n < outsz - 1)
    {
      out[n++] = *p++;
    }

  out[n] = '\0';
  return n;
}

/****************************************************************************
 * Name: gps_parse_coord
 *
 * Convert a ddmm.mmmm NMEA coordinate + hemisphere into decimal degrees.
 ****************************************************************************/

static bool gps_parse_coord(const char *coord, char hemi, double *out_deg)
{
  double v;
  int dd;
  double mm;

  if (coord == NULL || coord[0] == '\0')
    {
      return false;
    }

  v = strtod(coord, NULL);
  dd = (int)(v / 100.0);
  mm = v - (double)dd * 100.0;

  *out_deg = (double)dd + mm / 60.0;
  if (hemi == 'S' || hemi == 'W')
    {
      *out_deg = -*out_deg;
    }

  return true;
}

/****************************************************************************
 * Name: gps_process_sentence
 *
 * Parse a single NMEA sentence (line without trailing CR/LF).
 ****************************************************************************/

static void gps_process_sentence(char *line)
{
  char type[16];
  char f[64];

  gps_nth_field(line, 0, type, sizeof(type));

  if (strstr(type, "GGA") == NULL)
    {
      return;
    }

  gps_nth_field(line, 6, f, sizeof(f));
  g_fix.fix_quality = atoi(f);

  gps_nth_field(line, 7, f, sizeof(f));
  g_fix.sats = atoi(f);

  {
    char lat[32];
    char ns[2];
    char lon[32];
    char ew[2];
    double latd = 0.0;
    double lond = 0.0;
    bool latv;
    bool lonv;

    gps_nth_field(line, 2, lat, sizeof(lat));
    gps_nth_field(line, 3, ns, sizeof(ns));
    gps_nth_field(line, 4, lon, sizeof(lon));
    gps_nth_field(line, 5, ew, sizeof(ew));

    latv = gps_parse_coord(lat, ns[0], &latd);
    lonv = gps_parse_coord(lon, ew[0], &lond);

    g_fix.pos_valid = false;
    if (latv && lonv)
      {
        g_fix.lat_deg = latd;
        g_fix.lon_deg = lond;
        g_fix.pos_valid = true;
      }
  }

  g_gga_count++;
  if ((g_gga_count % 5) == 0)
    {
      if (g_fix.pos_valid)
        {
          syslog(LOG_INFO,
                 "GPS fix q=%d sats=%d lat=%.5f lon=%.5f\n",
                 g_fix.fix_quality, g_fix.sats,
                 g_fix.lat_deg, g_fix.lon_deg);
        }
      else
        {
          syslog(LOG_INFO, "GPS fix q=%d sats=%d (no position yet)\n",
                 g_fix.fix_quality, g_fix.sats);
        }
    }
}

/****************************************************************************
 * Name: gps_reader_thread
 ****************************************************************************/

static FAR void *gps_reader_thread(FAR void *arg)
{
  char buf[128];
  char line[256];
  int line_len = 0;
  int total = 0;
  ssize_t n;
  int i;

  for (; ; )
    {
      n = read(g_gps_fd, buf, sizeof(buf));
      if (n <= 0)
        {
          if (n < 0 && errno != EINTR)
            {
              syslog(LOG_ERR, "GPS: read failed: %d\n", errno);
              break;
            }

          continue;
        }

      total += (int)n;
      if ((total & 0x3ff) == 0)
        {
          syslog(LOG_INFO, "GPS rx total=%d bytes\n", total);
        }

      for (i = 0; i < (int)n; i++)
        {
          char c = buf[i];

          if (c == '\n' || c == '\r')
            {
              if (line_len > 0)
                {
                  line[line_len] = '\0';
                  gps_process_sentence(line);
                  line_len = 0;
                }

              continue;
            }

          if (line_len < (int)sizeof(line) - 1)
            {
              line[line_len++] = c;
            }
          else
            {
              /* Over-long line — drop it and resync on the next CR/LF. */
              line_len = 0;
            }
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: metalio_gps_initialize
 ****************************************************************************/

int metalio_gps_initialize(void)
{
  int ret;

  /* Power on the GNSS module (P0.0, high-enable). */

  metalio_tca9555_write_pin(BOARD_IOEXP_GPS_POWER, true);

#ifdef CONFIG_ESPRESSIF_UART0
  g_gps_fd = open(GPS_UART_DEVICE, O_RDONLY);
  if (g_gps_fd < 0)
    {
      int err = errno;
      syslog(LOG_ERR, "GPS: open(%s) failed: %d\n", GPS_UART_DEVICE, err);
      return -err;
    }

  ret = pthread_create(&g_gps_thread, NULL, gps_reader_thread, NULL);
  if (ret != 0)
    {
      syslog(LOG_ERR, "GPS: pthread_create failed: %d\n", ret);
      close(g_gps_fd);
      g_gps_fd = -1;
      return -ret;
    }

  syslog(LOG_INFO, "GPS power on, UART %s (TX=%d RX=%d) 9600\n",
         GPS_UART_DEVICE, BOARD_GPS_TX_GPIO, BOARD_GPS_RX_GPIO);
#else
  syslog(LOG_INFO, "GPS power on (UART disabled, TX=%d RX=%d)\n",
         BOARD_GPS_TX_GPIO, BOARD_GPS_RX_GPIO);
#endif

  return OK;
}

/****************************************************************************
 * Name: metalio_gps_get_snapshot
 ****************************************************************************/

int metalio_gps_get_snapshot(FAR int *fix_quality, FAR int *sats,
                             FAR double *lat, FAR double *lon)
{
  if (g_gga_count == 0)
    {
      return -EAGAIN;
    }

  if (fix_quality != NULL)
    {
      *fix_quality = g_fix.fix_quality;
    }

  if (sats != NULL)
    {
      *sats = g_fix.sats;
    }

  if (lat != NULL)
    {
      *lat = g_fix.pos_valid ? g_fix.lat_deg : 0.0;
    }

  if (lon != NULL)
    {
      *lon = g_fix.pos_valid ? g_fix.lon_deg : 0.0;
    }

  return OK;
}
