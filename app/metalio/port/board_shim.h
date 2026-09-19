/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board abstraction shim for NuttX/openvela.
 * Provides the Board singleton that the original xiaozhi-esp32 code expects.
 *
 * The heavy lifting (display, audio, network, backlight, LED) is delegated
 * to MetalioClaw4Board (boards/metalio_claw_4_board.h).  This header stays
 * free of LVGL / hardware includes so it can be included by translation
 * units that only need the Board interface.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef BOARD_TYPE
#define BOARD_TYPE "metalio-claw-4"
#endif
#ifndef BOARD_NAME
#define BOARD_NAME "metalio-claw-4"
#endif

#ifdef __cplusplus

#include <string>

/* Forward declarations — full definitions are pulled in only by the
 * translation units that actually use them (board_shim.cxx includes
 * metalio_claw_4_board.h). */
class Display;
class AudioService;
class LcdDisplay;
class Backlight;
class Led;

/**
 * Board singleton — matches the original xiaozhi-esp32 Board class API.
 * Each getter returns a pointer that may be nullptr until the subsystem
 * is initialized by Application::Start() (or by an explicit
 * Board::Initialize() call).
 */
class Board
{
public:
    static Board &GetInstance();

    /* Initialization — called by Application::Start() or metalio_app_start.
     * Delegates to MetalioClaw4Board::Initialize() which brings up the
     * display, audio, network, backlight and LED subsystems. */
    void Initialize();
    void InitializeDisplay();
    void InitializeAudio();
    void InitializeNetwork();

    /* Subsystem accessors (may return nullptr before init) */
    Display *GetDisplay();
    LcdDisplay *GetLcd();
    AudioService *GetAudioService();
    Backlight *GetBacklight();
    Led *GetLed();

    /* Hardware info */
    std::string GetBoardType() const;
    std::string GetFirmwareVersion() const;

    /* Device identity used by the xiaozhi.me OTA / activation flow.
     * UUID is generated once and persisted; system-info JSON is the
     * POST body that the official OTA endpoint expects. */
    std::string GetUuid();
    std::string GetSystemInfoJson();
    std::string GetBoardJson();

private:
    Board() = default;
    ~Board() = default;
    Board(const Board &) = delete;
    Board &operator=(const Board &) = delete;

    std::string uuid_;
    void EnsureUuid();
    static std::string GenerateUuid();
};

/* C-linkage accessor for use from metalio_main.c */
extern "C" void board_shim_initialize(void);

/* C-linkage hardware accessors (used by display/protocol code) */
extern "C" int  metalio_board_get_battery(int *level, int *charging, int *discharging);
extern "C" const char *metalio_board_get_network_icon(void);
extern "C" int  metalio_board_get_volume(int *volume);
extern "C" const char *metalio_board_get_network_state(void);

#endif /* __cplusplus */
