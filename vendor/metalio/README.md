# Metalio Claw4 openvela vendor overlay

- Board BSP: `nuttx/boards/risc-v/esp32p4/metalio-claw-4`
- Drivers: `vendor/metalio/drivers` (synced into board `src/metalio` for linking)
- Reference firmware: `third_party/MetalioClaw4`
- Apps: `apps/metalio`

## Build

```bash
cd /path/to/HiOS
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/nsh -j$(nproc)
# or wifi / lvgl / ai configs
```

## Flash

```bash
cd nuttx
make flash ESPTOOL_PORT=/dev/ttyACM0 ESPTOOL_BINDIR=./
```
