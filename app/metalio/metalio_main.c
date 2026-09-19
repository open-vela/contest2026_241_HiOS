/****************************************************************************
 * Metalio Claw4 application entry (openvela / NuttX)
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <unistd.h>

int metalio_app_start(int argc, char *argv[]);

#ifdef CONFIG_BUILD_KERNEL
int main(int argc, FAR char *argv[])
#else
int metalio_main(int argc, char *argv[])
#endif
{
  write(1, "MAIN0\n", 6);
  return metalio_app_start(argc, argv);
}
