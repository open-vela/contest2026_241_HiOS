---
name: openvela-board-porting
description: 将未受官方支持的新硬件（SoC + 板卡）移植到 openvela（NuttX）的端到端方法。当需要新增板级 BSP、编写 defconfig、移植设备驱动、解决 HAL API 漂移或链接错误、完成编译烧录验证时使用。
---

# openvela 新硬件适配

将一款未受官方支持的硬件（SoC + 板卡）完整移植到 openvela（基于 NuttX）的方法论。
本 skill 提炼自 HiOS「Metalio Claw4（ESP32-P4 + ESP32-C5）」的实战经验。

## 三层结构

一次完整适配覆盖三层，通过本仓 `<linkfile>` 软链进 openvela 编译树：

| 层 | 目录 | 映射到 openvela 树 |
| --- | --- | --- |
| 板级 BSP | `board/<board-name>` | `nuttx/boards/<arch>/<chip>/<board-name>` |
| 驱动 | `vendor/<name>` | `vendor/<name>` |
| 应用 | `app/<name>` | `apps/<name>` |

## 移植步骤

### 0. 树与工具链
- `repo init` + `repo sync` 同步 openvela 基线（如 `tags/trunk-5.5.xml`）。
- 确认 RISC-V 工具链（`riscv32-esp-elf` 14.2，位于 `~/.espressif`）与 `esptool.py` 已安装。
- 若芯片在基线中不存在，先从 Apache NuttX master **回移植**该芯片/板卡支持。

### 1. 板级 BSP
- 在 `nuttx/boards/<arch>/<chip>/` 下新建板卡目录，编写 `include/board.h`（完整 pin-map）。
- 按形态各建一个 defconfig：`nsh`（最小）、`wifi`、`lvgl`、`ai` 等。
- `src/Make.defs` 与 `src/CMakeLists.txt` **必须同步维护**，否则 CMake 构建会漏编驱动导致链接失败。
- `src/<board>_bringup.c` 统一注册驱动；`#ifdef A || B` 要写成 `#if defined(A) || defined(B)`。

### 2. 驱动
- 每个驱动实现 `initialize` 并在 bringup 中注册。
- 新驱动在 Kconfig 中用 `depends on VIDEO_FB` 之类的约束，避免「依赖未开启」时产生未定义符号。
- 源码补正确的 `#include`（如 `nuttx/mutex.h`），不要依赖 `-Wno-implicit-function-declaration` 掩盖问题。

### 3. 应用
- 在 `apps/<name>` 下编写 POSIX 应用，Makefile 引入 `APPDIR`。

### 4. 编译
```bash
./build.sh nuttx/boards/<arch>/<chip>/<board>/configs/nsh -j$(nproc)
# 或
cd nuttx && make -j$(nproc)
```

### 5. 烧录
```bash
cd nuttx
esptool.py --chip <chip> --port /dev/ttyACM0 --baud 921600 \
    --before default_reset --after hard_reset --no-stub \
    write_flash --verify -fs 32MB -fm dio -ff 40m 0x2000 nuttx.bin
```

### 6. 验证
- 抓串口启动日志，确认关键标记（`*** Booting NuttX ***` → `Board init done` → 应用 ready）。
- `strings nuttx.bin | grep <keyword>` 校验固件内容。

## 常见坑（HAL API 漂移）

- **工具链错误**：defconfig 默认 `RISCV_TOOLCHAIN_GNU_RV64`（`riscv64-unknown-elf`），旧版 10.2 会报 `zifencei` / `__atomic_fetch_or_8` 链接错误。改用 `riscv32-esp-elf` 14.2：`make CROSSDEV=riscv32-esp-elf-`。
- **rv32 无 64 位原子**：`riscv32-esp-elf` 不带 `libatomic.a`，rv32imac 无 64 位原子指令。把 `_Atomic uint64_t` 改成 `up_irq_save/restore` 保护的 RMW（仅 `CONFIG_SMP=n` 时安全）。
- **`nxtask_init` 签名漂移**：trunk 迁移为 posix_spawn 化签名，改用 `posix_spawnattr_t`。
- **缺头文件**：`O_NONBLOCK` / `F_SETFL` 需要 `<fcntl.h>`。
- **`fb_register_device` 未定义**：显示驱动 `depends on VIDEO_FB`，nsh 下未开 `VIDEO_FB` 时不编显示驱动。
- **`nxmutex_init/destroy` 未定义**：`esp_lowputc.c` 缺 `#include <nuttx/mutex.h>`。
- **`make distclean` 会删 `esp-hal-3rdparty`**（build 时克隆）：用 `make clean`；HAL 缺失需重新克隆并重打补丁。

## 验收清单

按 Phase 逐项勾选（树/工具链 → bringup → 电源/I2C → 显示/触摸 → Wi-Fi → 存储/音频/传感器 → 应用 → 量产），每项区分「代码就绪」与「硬件实测通过」。
