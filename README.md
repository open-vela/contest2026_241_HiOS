# HiOS — Metalio Claw4 on openvela

## 一、作品简介

将 **Metalio Claw4**（ESP32-P4 主控 + ESP32-C5 协处理器）AI 硬件完整移植到 **openvela**（trunk-5.5）操作系统，交付一套从底层 BSP、驱动到上层应用的端到端适配：

- **板级 BSP**：在 openvela / NuttX 上新增 `metalio-claw-4` 板卡，提供 5 套配置（`nsh` / `wifi` / `lvgl` / `ai` / `i2c`）与完整 pin-map。
- **设备驱动**：TCA9555（GPIO 扩展）、BQ27220（电量计）、NU1680、NV3051F（720×720 显示）、GT911（触摸）、SD、GPS、I2S / BT 音频、摄像头、双网络切换、OTA/待机等 15+ 驱动。
- **应用层**：`apps/metalio` POSIX 应用 —— OpenClaw 客户端、本地对话、网络服务、MCP、UI、音频、i18n 等。
- **联网方案**：ESP-Hosted Wi-Fi（P4↔C5 SDIO），实现 host 侧 `wlan0` 网络栈。

亮点：ESP32-P4 芯片/板卡支持从 Apache NuttX master 回移植；针对 openvela trunk-5.5 的 HAL API 漂移（`nxtask_init`→posix_spawn 化、rv32 无 64 位原子等）做了源码级修复并记录于 `docs/porting/acceptance.md`。

## 二、选题方向

**新硬件适配** —— 将官方未支持的 Metalio Claw4（ESP32-P4 + ESP32-C5）完整移植到 openvela，覆盖 BSP、驱动、应用三层，并以「可复现的验收清单」记录移植过程中的关键问题与修复。

## 三、目录结构

```
├── board/metalio-claw-4/   # 板级 BSP：defconfigs(nsh/wifi/lvgl/ai/i2c) + src 驱动 + 链接脚本
├── vendor/metalio/         # 驱动源码（drivers/）+ 头文件（include/metalio/metalio.h）
├── app/metalio/            # 应用层（OpenClaw / MCP / UI / audio / network / i18n）
├── docs/porting/           # 移植说明 + 验收清单（acceptance.md）
├── scripts/                # sync_metalio_drivers.sh：vendor 驱动同步进 board 包
├── .claude/skills/         # 自建 skill：openvela-board-porting（适配方法论）、metalio-mcp-device-control（设备控制 MCP）
├── logs/                   # AI Coding 日志（另行提交）
└── contest2026_241_HiOS.xml # manifest：<linkfile> 将上述目录软链进 openvela 编译树
```

软链映射（见 `contest2026_241_HiOS.xml`）：

| 本仓目录 | 映射到 openvela 编译树 |
| --- | --- |
| `board/metalio-claw-4` | `nuttx/boards/risc-v/esp32p4/metalio-claw-4` |
| `vendor/metalio` | `vendor/metalio` |
| `app/metalio` | `apps/metalio` |

## 四、运行方式

前置：openvela trunk-5.5 工程已 `repo init` + `repo sync`；RISC-V 工具链（`riscv32-esp-elf` 14.2，位于 `~/.espressif`）与 `esptool.py` 已安装。

### 环境准备

| 工具 | 用途 |
|------|------|
| `make` | NuttX 编译 |
| `riscv32-esp-elf` 14.2（`~/.espressif`） | RISC-V 交叉编译 |
| `esptool.py` | 烧录 ESP32-P4 |
| `python3` + `pyserial` | 抓取串口日志 |

```bash
pip install esptool pyserial
export PATH="$HOME/.espressif/tools/riscv32-esp-elf/14.2.0_*/riscv32-esp-elf/bin:$PATH"
```

将 ESP32-P4 经 USB 连接主机，确认调试串口（通常为 `/dev/ttyACM0`，厂商信息应为 `Espressif` / `USB_JTAG_serial_debug_unit`）：

```bash
ls /dev/ttyACM* /dev/ttyUSB*
```

### 编译

```bash
# 方式一：openvela 统一入口（在 openvela 工作区根目录，board config 四选一）
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/nsh  -j$(nproc)
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/wifi -j$(nproc)
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/lvgl -j$(nproc)
./build.sh nuttx/boards/risc-v/esp32p4/metalio-claw-4/configs/ai   -j$(nproc)

# 方式二：.config 已配置时，进入 nuttx 直接 make
cd nuttx
make -j$(nproc)
```

编译成功生成 `nuttx/nuttx.bin`，末尾打印内存占用（`irom_seg` / `drom_seg` / `sram_low` 等）。修改生成头文件（如 `i18n_strings_gen.h`）后若 make 未生效，删除对应 `.o` 再编译：

```bash
rm -f apps/metalio/i18n/i18n.cxx.*.o
make -j$(nproc)
```

### 烧录

```bash
cd nuttx

# 方式一：项目脚本（esp32p4 / 921600 / 32MB / DIO / 40MHz / 偏移 0x2000）
./flash_espclaw.sh /dev/ttyACM0

# 方式二：手动 esptool（等价命令）
esptool.py --chip esp32p4 --port /dev/ttyACM0 --baud 921600 \
    --before default_reset --after hard_reset --no-stub \
    write_flash --verify -fs 32MB -fm dio -ff 40m 0x2000 nuttx.bin
```

### 验证

```bash
python3 capture_acm0.py                 # 抓取 45s 启动日志
strings nuttx.bin | grep "Hi openvela"  # 校验唤醒词
```

启动日志应依次出现：`*** Booting NuttX ***` → `Board init done` → `SR_OK` → `NETWIFI dhcp_ret=0` → `HOME_OK` → `Xiaozhi boot ready`。

### 注意

- 默认 defconfig 选择 `RISCV_TOOLCHAIN_GNU_RV64`（`riscv64-unknown-elf`），旧版 10.2 会因 `zifencei`/rv32 原子 helper 报错，请改用 `riscv32-esp-elf` 14.2（或用 `make CROSSDEV=riscv32-esp-elf-`）。
- 不要执行 `make distclean`（会删掉 build 克隆的 `esp-hal-3rdparty`），用 `make clean`。
- ESP32-C5 需保持 Espressif/Metalio 的 ESP-Hosted 从机固件；P4 host 复位 GPIO54 为高有效。
- `flash_espclaw.sh` / `capture_acm0.py` 等辅助脚本位于 openvela 工程 `nuttx/` 目录，未随本仓提交。

完整移植细节与逐项验收见 `docs/porting/acceptance.md`。

## 五、AI Coding 使用说明

本作品在需求拆解、方案设计、驱动/BSP 编码、链接排错与文档各环节均借助 AI（Claude Code）辅助开发：

- **方案设计**：梳理 ESP32-P4 回移植与 ESP-Hosted 联网的架构取舍；
- **编码**：板级 BSP、15+ 驱动、应用层代码的生成与审查；
- **调试**：nsh 链接阻塞（`nxmutex`/`fb_register_device`/`zifencei`/`__atomic_fetch_or_8`）定位与修复，见 `docs/porting/acceptance.md`；
- **Skill 化**：将移植方法论沉淀为自建 skill `openvela-board-porting`（见 `.claude/skills/`），供后续新硬件适配复用；
- **文档**：验收清单与移植说明撰写。

完整对话日志见 `logs/` 目录。
