/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Umbrella header for the Metalio Claw4 application (openvela port).
 *
 * This header aggregates the public interfaces of the ported application
 * modules so that callers — and the C entry point in metalio_main.c — can
 * pull in the full C++ API with a single include. The real implementations
 * live in:
 *
 *   - application.h         Application orchestrator + state machine
 *   - openclaw_client.h     OpenClaw cloud HTTP/WebSocket client
 *   - network_service.h     WiFi / cellular bring-up
 *   - mcp_server.h          Model Context Protocol server (JSON-RPC)
 *   - protocol.h            Protocol abstraction (MQTT / WebSocket)
 *   - ota.h                 Firmware update flow
 *   - audio_service.h       Audio capture / playback / OPUS pipeline
 *   - device_state.h        Device state enum
 *
 * ESP-IDF APIs are shimmed — see port/*.h for details.  Display / LVGL
 * headers (display.h, lvgl_display.h, ...) are NOT included here because
 * they pull in <lvgl.h>; consumers that need the display include it
 * directly.
 *
 * Note: application.h transitively includes protocol.h, ota.h,
 * audio_service.h and device_state.h, so they are not re-listed below.
 */

#ifndef METALIO_APP_H
#define METALIO_APP_H

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#ifdef __cplusplus

/* Core application modules.  application.h pulls in protocol.h, ota.h,
 * audio_service.h and device_state.h transitively. */
#include "application.h"
#include "openclaw_client.h"
#include "network_service.h"
#include "mcp_server.h"

#endif /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * C-linkage entry point invoked by metalio_main.c (the NuttX application
 * start hook).  Implemented in application.cxx as
 *   Application::GetInstance().Start();
 */
int metalio_app_start(int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif /* METALIO_APP_H */
