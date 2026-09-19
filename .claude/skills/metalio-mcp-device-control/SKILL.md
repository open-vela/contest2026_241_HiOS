---
name: metalio-mcp-device-control
description: Metalio 设备控制的 MCP 工具化设计（运行时 skill）及其开发阶段的编译/烧录/联调 skill。当需要把本机、DLP 节点、HA 实体注册为 MCP 工具，或进行 flash-espclaw 烧录、hios-build-p4 编译与串口联调时使用。
---

# Metalio 设备控制（metalio-mcp-device-control）

## 运行时 Skill：设备控制

将本机、DLP 节点和 HA 实体注册为 MCP 工具：

- **命名**：遵循 3.2 节的 `self.*`、`home.<位置>.<设备>.*`、`ha.<实体>.*`。
- **消歧**：描述中包含位置和别名，用于区分同名设备。

### 触发

用户在聊天状态中表达控制意图：

- 本地 `ChatLocalCmd` 命中 → 由端侧立即执行；
- 未命中 → 由云端 `tools/call` 路由到注册中心。

场景一至场景四均为触发示例。

### 运行位置

`apps/metalio` 的 MCP Server：

- 本机工具在编译期注册；
- DeviceLink 节点上线后动态 `AddTool`。

此为固件内的运行时注册。

## 开发阶段 Skill（仅用于编译、烧录和联调）

- **flash-espclaw**：使用 `flash_espclaw.sh` 烧录 `nuttx.bin`，并抓取串口。
- **hios-build-p4**：编译时指定 `CROSSDEV` 和 `EXTRAFLAGS`，避免因工具链错误导致「能编译但无法运行」。

### 联调约定（串口标记）

| 标记 | 含义 |
| --- | --- |
| `MCP_LED_ON` | 云端工具调用 |
| `DL_LISTEN` | DeviceLink 监听 |
| `LOC_*` | 本地命令命中 |
