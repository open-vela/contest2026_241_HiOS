/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/src/esp32p4_appinit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <nuttx/board.h>
#include <nuttx/compiler.h>

#include "metalio-claw-4.h"

#ifdef CONFIG_BOARDCTL

/****************************************************************************
 * Name: board_app_initialize
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
  UNUSED(arg);

#ifndef CONFIG_BOARD_LATE_INITIALIZE
  return esp_bringup();
#else
  return OK;
#endif
}

#endif /* CONFIG_BOARDCTL */
