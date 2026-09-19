/*
 * __cxa_guard_acquire / __cxa_guard_release replacement for NuttX/openvela.
 *
 * The default libsupc++ versions use pthread_mutex / pthread_cond which
 * may hang during early startup on NuttX.  This simplified version
 * just checks the guard byte without any multi-threading protection —
 * safe for single-threaded embedded systems like ESP32-P4.
 */

#include <nuttx/compiler.h>

/* Standard C++ ABI: guard is a 64-bit variable; first byte = 0 before
 * initialization, 1 after.  We only read/write the first byte. */

__extension__ typedef int __guard __attribute__((mode(__DI__)));

int __cxa_guard_acquire(FAR __guard *g)
{
  return !*(FAR char *)g;
}

void __cxa_guard_release(FAR __guard *g)
{
  *(FAR char *)g = 1;
}

void __cxa_guard_abort(FAR __guard *g)
{
  (void)g;
}
