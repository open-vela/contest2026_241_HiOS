# ##############################################################################
# boards/risc-v/esp32p4/metalio-claw-4/src/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

set(SRCS esp32p4_boot.c esp32p4_bringup.c)

if(CONFIG_BOARDCTL)
  if(CONFIG_BOARDCTL_RESET)
    list(APPEND SRCS esp32p4_reset.c)
  endif()
endif()

if(CONFIG_DEV_GPIO)
  list(APPEND SRCS esp32p4_gpio.c)
endif()

if(CONFIG_ARCH_BUTTONS)
  list(APPEND SRCS esp32p4_buttons.c)
endif()

if(CONFIG_ESPRESSIF_EMAC)
  list(APPEND SRCS esp32p4_ethernet.c)
endif()

# ##############################################################################
# Metalio vendor drivers (mirror src/Make.defs; synced from vendor/metalio/drivers
# into src/metalio by scripts/sync_metalio_drivers.sh)
# ##############################################################################

list(APPEND SRCS metalio/tca9555.c metalio/bq27220.c metalio/nu1680.c
            metalio/cx25601n.c)
list(APPEND SRCS metalio/sc7a20h.c metalio/qmc6309.c)
list(APPEND SRCS metalio/sdcard.c metalio/gps.c metalio/bt_audio.c)
list(APPEND SRCS metalio/nt26.c metalio/camera.c metalio/dual_network.c
            metalio/ota_standby.c)

if(CONFIG_METALIO_TOUCH_GT911)
  list(APPEND SRCS metalio/gt911.c)
endif()

if(CONFIG_VIDEO_FB)
  if(CONFIG_METALIO_DISPLAY_NV3051F OR CONFIG_METALIO_DISPLAY_FL7707N)
    list(APPEND SRCS metalio/display_nv3051f.c)
  endif()
endif()

if(CONFIG_NET AND CONFIG_METALIO_ESP_HOSTED)
  list(APPEND SRCS metalio/esp_hosted/esp_hosted_netdev.c)
endif()

target_sources(board PRIVATE ${SRCS})

# ##############################################################################
# Linker scripts (P4: sections or sections.rev3 when REV_MIN_300)
# ##############################################################################

string(REPLACE "\"" "" CHIP_SERIES "${CONFIG_ESPRESSIF_CHIP_SERIES}")

set(BOARD_COMMON_DIR "${NUTTX_BOARD_DIR}/../common")

set(LDSCRIPTS "${BOARD_COMMON_DIR}/scripts/${CHIP_SERIES}_aliases.ld"
              "${BOARD_COMMON_DIR}/scripts/${CHIP_SERIES}_flat_memory.ld")

if(CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT OR CONFIG_ESPRESSIF_SIMPLE_BOOT)
  if(NOT "${CONFIG_ESP32P4_SELECTS_REV_LESS_V3}" STREQUAL "y")
    list(APPEND LDSCRIPTS
         "${BOARD_COMMON_DIR}/scripts/${CHIP_SERIES}_sections.rev3.ld")
  else()
    list(APPEND LDSCRIPTS
         "${BOARD_COMMON_DIR}/scripts/${CHIP_SERIES}_sections.ld")
  endif()
endif()

set_property(GLOBAL APPEND PROPERTY LD_SCRIPT ${LDSCRIPTS})
