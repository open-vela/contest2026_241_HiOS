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
#include <time.h>
#include <stdio.h>

#include <arch/board/board.h>
#include <metalio/metalio.h>

#define GPS_UART_DEVICE "/dev/ttyS0"

static int g_gps_fd = -1;
static pthread_t g_gps_thread;

static struct metalio_gps_snapshot_s g_fix;
static int g_gga_count;
static pthread_mutex_t g_fix_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fix_ms,g_speed_ms;
static int g_visible[7];
static uint64_t g_visible_ms[7];
static uint64_t gps_now_ms(void) {
  struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

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

  char *end;
  if (hemi!='N' && hemi!='S' && hemi!='E' && hemi!='W') return false;
  v = strtod(coord, &end);
  if (*end || !__builtin_isfinite(v) || v<0) return false;
  dd = (int)(v / 100.0);
  mm = v - (double)dd * 100.0;
  if(mm>=60 || dd>((hemi=='N'||hemi=='S')?90:180)) return false;
  *out_deg = (double)dd + mm / 60.0;
  if(*out_deg>((hemi=='N'||hemi=='S')?90:180)) return false;
  if (hemi == 'S' || hemi == 'W') *out_deg = -*out_deg;

  return true;
}

/****************************************************************************
 * Name: gps_process_sentence
 *
 * Parse a single NMEA sentence (line without trailing CR/LF).
 ****************************************************************************/

static bool gps_checksum(const char *line) {
  if(!line || *line!='$') return false;
  const char *star=strchr(line,'*');
  if(!star || strlen(star)!=3) return false;
  unsigned actual=0;
  for(const char *p=line+1;p<star;++p) actual^=(unsigned char)*p;
  char *end;
  unsigned expected=strtoul(star+1,&end,16);
  return *end=='\0' && actual==expected;
}
static bool gps_number(const char *text,double low,double high,double *value) {
  char *end;
  if(!text || !text[0]) return false;
  double parsed=strtod(text,&end);
  if(*end || !__builtin_isfinite(parsed) || parsed<low || parsed>high) return false;
  *value=parsed;return true;
}
static void gps_process_sentence(char *line) {
  char type[16],f[64];
  if(!gps_checksum(line)) return;
  gps_nth_field(line,0,type,sizeof(type));
  if(strlen(type)!=5) return;
  pthread_mutex_lock(&g_fix_lock);
  if(strcmp(type+2,"GGA")==0) {
    char lat[32],lon[32],ns[2],ew[2];double latd=0,lond=0;
    gps_nth_field(line,6,f,sizeof(f));g_fix.fix_quality=atoi(f);
    if(g_fix.fix_quality<0 || g_fix.fix_quality>8) g_fix.fix_quality=0;
    gps_nth_field(line,7,f,sizeof(f));g_fix.sats=atoi(f);
    if(g_fix.sats<0 || g_fix.sats>99) g_fix.sats=0;
    gps_nth_field(line,2,lat,sizeof(lat));gps_nth_field(line,3,ns,sizeof(ns));
    gps_nth_field(line,4,lon,sizeof(lon));gps_nth_field(line,5,ew,sizeof(ew));
    g_fix.position_valid=g_fix.fix_quality>0 && gps_parse_coord(lat,ns[0],&latd) && gps_parse_coord(lon,ew[0],&lond);
    if(g_fix.position_valid) { g_fix.latitude=latd;g_fix.longitude=lond; }
    gps_nth_field(line,8,f,sizeof(f));
    if(!gps_number(f,0,100,&g_fix.hdop)) g_fix.hdop=0;
    gps_nth_field(line,9,f,sizeof(f));
    g_fix.altitude_valid=g_fix.position_valid && gps_number(f,-1000,50000,&g_fix.altitude_m);
    gps_nth_field(line,1,g_fix.utc_time,sizeof(g_fix.utc_time));
    g_fix_ms=gps_now_ms();++g_gga_count;
  } else if(strcmp(type+2,"RMC")==0) {
    gps_nth_field(line,2,f,sizeof(f));
    bool valid=f[0]=='A';
    gps_nth_field(line,7,f,sizeof(f));
    g_fix.speed_valid=valid && gps_number(f,0,2000,&g_fix.speed_kmh);g_speed_ms=gps_now_ms();
    if(g_fix.speed_valid) g_fix.speed_kmh*=1.852;
    if(!valid) {g_fix.fix_quality=0;g_fix.position_valid=false;}
  } else if(strcmp(type+2,"GSV")==0) {
    int index=-1;
    const char *talkers[]={"GN","GP","GB","GL","GA","GQ","GI"};
    for(int i=0;i<7;++i) if(strncmp(type,talkers[i],2)==0) index=i;
    if(strncmp(type,"BD",2)==0) index=2;
    gps_nth_field(line,3,f,sizeof(f));int count=atoi(f);
    if(index>=0 && count>=0 && count<=99) {g_visible[index]=count;g_visible_ms[index]=gps_now_ms();}
  }
  pthread_mutex_unlock(&g_fix_lock);
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

int metalio_gps_get_snapshot_ex(FAR struct metalio_gps_snapshot_s *snapshot)
{
  if(snapshot==NULL) return -EINVAL;
  pthread_mutex_lock(&g_fix_lock);
  if(g_gga_count==0) {pthread_mutex_unlock(&g_fix_lock);return -EAGAIN;}
  *snapshot=g_fix;
  uint64_t now=gps_now_ms();snapshot->satellites_view=0;
  if(g_visible_ms[0] && now-g_visible_ms[0]<=5000) snapshot->satellites_view=g_visible[0];
  else for(int i=1;i<7;++i) if(g_visible_ms[i] && now-g_visible_ms[i]<=5000) snapshot->satellites_view+=g_visible[i];
  if(gps_now_ms()-g_speed_ms>5000) snapshot->speed_valid=false;
  if(gps_now_ms()-g_fix_ms>5000) {
    snapshot->fix_quality=0;snapshot->position_valid=false;
    snapshot->altitude_valid=false;snapshot->speed_valid=false;
  }
  pthread_mutex_unlock(&g_fix_lock);
  return OK;
}
int metalio_gps_get_snapshot(FAR int *fix_quality,FAR int *sats,FAR double *lat,FAR double *lon)
{
  struct metalio_gps_snapshot_s snapshot;
  int rc=metalio_gps_get_snapshot_ex(&snapshot);
  if(rc<0) return rc;
  if(fix_quality) *fix_quality=snapshot.position_valid?snapshot.fix_quality:0;
  if(sats) *sats=snapshot.sats;
  if(lat) *lat=snapshot.position_valid?snapshot.latitude:0;
  if(lon) *lon=snapshot.position_valid?snapshot.longitude:0;
  return OK;
}
