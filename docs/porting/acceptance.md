# Metalio Claw4 openvela Port — Acceptance Checklist

## Phase 0 — Tree / Toolchain
- [x] openvela trunk-5.5 synced (nuttx/apps/build/docs/vendor)
- [x] ESP32-P4 backported from apache/nuttx
- [x] `third_party/MetalioClaw4` reference tree present
- [x] RISC-V toolchain + esptool installed (riscv32-esp-elf 14.2.0 @ `~/.espressif`; `esptool.py`)
- [x] `metalio-claw-4:nsh` configures (verified)

> Toolchain note: the defconfig defaults to `RISCV_TOOLCHAIN_GNU_RV64` which picks
> `riscv64-unknown-elf`. The host's 10.2 build is too old (binutils rejects `zifencei`,
> missing rv32 64-bit atomic helpers). Build with the ESP toolchain instead:
> `make CROSSDEV=riscv32-esp-elf-` with `~/.espressif/tools/riscv32-esp-elf/.../bin` on PATH.

## Build / link status (nsh)

Four link blockers found during first full build; three fixed in-tree:

- [x] `nxmutex_init/destroy` undefined — `arch/risc-v/src/common/espressif/esp_lowputc.c`
      was missing `#include <nuttx/mutex.h>` (masked by `-Wno-implicit-function-declaration`).
      Fixed by adding the include.
- [x] `fb_register_device` undefined — `METALIO_DISPLAY_NV3051F` defaulted `y` so the display
      driver compiled under nsh (which lacks `VIDEO_FB`). Fixed by adding
      `depends on VIDEO_FB` to the board Kconfig + a `VIDEO_FB` guard in `src/Make.defs`
      and `src/CMakeLists.txt`.
- [x] `zifencei` / `__atomic_fetch_or_8` link errors — caused by the wrong/old toolchain;
      resolved by switching to `riscv32-esp-elf 14.2` (see toolchain note above).
- [x] CMake build path (`build.sh --cmake`) omitted all Metalio vendor drivers from
      `src/CMakeLists.txt` → would fail to link `metalio_*_initialize`. Fixed by mirroring
      `src/Make.defs` in `src/CMakeLists.txt`.
- [x] **HAL API drift vs openvela trunk-5.5** (surfaced once the right toolchain + HAL clone
      were in place): `nxtask_init()` migrated to posix_spawn-based signature; `O_NONBLOCK` /
      `F_SETFL` not visible without `<fcntl.h>`; `__atomic_fetch_or_8` undefined because
      riscv32-esp-elf ships no libatomic.a and rv32imac has no 64-bit atomics. Fixed in
      `esp-hal-3rdparty/nuttx/src/platform/os.c` (add `<fcntl.h>`, switch to posix_spawnattr_t)
      and `esp-hal-3rdparty/components/esp_hw_support/esp_gpio_reserve.c` (replace C11
      `_Atomic uint64_t` with `up_irq_save/restore`-protected RMW; safe because CONFIG_SMP=n).
      Also fixed `#ifdef A || B` syntax in `esp32p4_bringup.c`.
- [x] **nsh build link verified** — `nuttx` ELF (583 KB) + `nuttx.bin` (274 KB) produced with
      `riscv32-esp-elf 14.2`. Memory: irom 151 KB / drom 242 KB / sram_low 34 KB.

> **Never `make distclean`** — it deletes the build-cloned `esp-hal-3rdparty`. Use `make clean`.
> If the HAL is missing, re-clone (network required) at commit `8d0a898910084206721a0892ab093021bca1496a`,
> then re-apply the os.c / esp_gpio_reserve.c fixes above (or keep a local patch set).

## Phase 1 — Bringup
- [x] Board `metalio-claw-4` with pin map in `board.h`
- [x] `configs/nsh/defconfig`
- [ ] Device boots to `nsh>`
- [ ] `uname` / `free` / I2C scan OK

## Phase 2 — Power / I2C
- [x] TCA9555 / BQ27220 / NU1680 drivers
- [x] Soft power-off pulse helper
- [ ] I2C devices detected on hardware
- [ ] Fuel gauge SOC / voltage readable

## Phase 3 — Display / Touch
- [x] NV3051F FB path + GT911 `/dev/input0`
- [x] `configs/lvgl/defconfig`
- [ ] 720×720 panel lit
- [ ] Touch coordinates validated

## Phase 4 — ESP-Hosted Wi-Fi
- [x] Host netdev skeleton (`wlan0`) + SDIO pin map
- [x] `configs/wifi/defconfig`
- [ ] C5 slave firmware flashed
- [ ] SDIO link up, DHCP, ping

## Phase 5 — Storage / Audio / Sensors
- [x] SD / GPS / I2S / BT audio / stubs
- [ ] SD mount works
- [ ] GPS NMEA stream
- [ ] BT codec AT OK

## Phase 6 — Application
- [x] `apps/metalio` Application / OpenClaw / MCP / UI / Audio / Network
- [x] `configs/ai/defconfig`
- [ ] Desktop + Wi-Fi provision + OpenClaw dialogue

## Phase 7 — Productization
- [x] NT26 / camera stubs
- [ ] Dual-network switch
- [ ] OTA / standby / i18n / USB MSC
