# Metalio Claw4 → openvela Porting Notes

## Layout

| Path | Role |
|------|------|
| `nuttx/boards/risc-v/esp32p4/metalio-claw-4` | Board BSP + defconfigs |
| `vendor/metalio` | Driver sources / overlay docs |
| `apps/metalio` | POSIX application (OpenClaw / UI) |
| `third_party/MetalioClaw4` | Upstream ESP-IDF reference (read-only) |

## Baseline

- openvela manifests: `tags/trunk-5.5.xml` (Gitee)
- ESP32-P4 chip/board support backported from Apache NuttX master
- Networking priority: ESP-Hosted Wi-Fi (P4↔C5 SDIO)

## Build examples

```bash
export PATH="/opt/riscv-none-elf-gcc/bin:$PATH"   # or riscv32-esp-elf
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/nsh -j$(nproc)
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/wifi -j$(nproc)
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/lvgl -j$(nproc)
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/ai -j$(nproc)
```

## Flash C5 slave

Keep Espressif/Metalio ESP-Hosted slave image on ESP32-C5. P4 host RESET GPIO54 is active-high.
