#include <nuttx/config.h>
#include <syslog.h>
#include <errno.h>
#include <metalio/metalio.h>

int metalio_ota_check(const char *url)
{
  syslog(LOG_INFO, "OTA: check %s (slot A/B TBD)\n", url ? url : "(null)");
  return OK;
}

int metalio_standby_enter(void)
{
  syslog(LOG_INFO, "standby: enter clock UI / idle power policy\n");
  return OK;
}
