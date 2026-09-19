#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/nuttx/boards/risc-v/esp32p4/metalio-claw-4/src/metalio"
mkdir -p "$ROOT/nuttx/boards/risc-v/esp32p4/metalio-claw-4/include/metalio"
cp -a "$ROOT/vendor/metalio/drivers/"*.c \
      "$ROOT/nuttx/boards/risc-v/esp32p4/metalio-claw-4/src/metalio/"
cp -a "$ROOT/vendor/metalio/drivers/esp_hosted" \
      "$ROOT/nuttx/boards/risc-v/esp32p4/metalio-claw-4/src/metalio/"
cp -a "$ROOT/vendor/metalio/include/metalio/metalio.h" \
      "$ROOT/nuttx/boards/risc-v/esp32p4/metalio-claw-4/include/metalio/"
echo "Synced vendor/metalio drivers into board package"
