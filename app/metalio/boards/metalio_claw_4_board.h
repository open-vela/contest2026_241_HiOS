/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MetalioClaw4Board — ported from MetalioClaw4
 * main/boards/metalio-claw-4/metalio-claw-4.cc + config.h.
 *
 * On ESP-IDF the board class (METALIO_CLAW_4) inherits from DualNetworkBoard
 * and performs the full hardware bring-up: I2C, IO expander, MIPI-DSI panel,
 * GT911 touch, BQ27220 fuel gauge, CX25601N charger, SD card, BT audio, etc.
 *
 * On NuttX the low-level hardware bring-up is done by the board library
 * (nuttx/boards/risc-v/esp32p4/metalio-claw-4/src/esp32p4_bringup.c) before
 * the userland application starts.  MetalioClaw4Board therefore focuses on
 * the application-level wiring that the bringup does NOT cover:
 *
 *   - Constructing the app-level Display (LvglDisplay / NoDisplay fallback)
 *   - Bringing up the NetworkService (WiFi via esp-hosted / 4G via NT26)
 *   - Constructing the Backlight control (PWM stub until a NuttX PWM driver
 *     is available)
 *   - Constructing the LED driver (SingleLed / GpioLed / NoLed)
 *   - Spawning the system-monitor task (CPU / memory / battery logging)
 *
 * The existing port/board_shim.cxx Board singleton delegates to this class
 * so Application::Start() sees a fully initialised board.
 */

#ifndef METALIO_CLAW_4_BOARD_H
#define METALIO_CLAW_4_BOARD_H

#include "display.h"
#include "led.h"
#include "esp_log_shim.h"

#include <string>

/* ================================================================== */
/* Board pin / timing configuration                                   */
/*                                                                    */
/* Ported from MetalioClaw4 main/boards/metalio-claw-4/config.h.      */
/* GPIO numbers match the ESP32-P4 datasheet; on NuttX they are       */
/* consumed only by the NuttX board library bringup (which runs        */
/* before this class).  They are kept here for reference and for      */
/* the LED / backlight drivers once NuttX GPIO/PWM userland drivers   */
/* are available.                                                     */
/* ================================================================== */

/* Audio */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

/* I2S pins */
#define AUDIO_I2S_MIC_GPIO_WS    10
#define AUDIO_I2S_MIC_GPIO_DIN   11
#define AUDIO_I2S_SPK_GPIO_DOUT  9
#define AUDIO_I2S_SPK_GPIO_BCLK  12

/* I2C (board main bus, port 1) */
#define I2C_SDA_PIN  7
#define I2C_SCL_PIN  8

/* Display */
#define DISPLAY_WIDTH   720
#define DISPLAY_HEIGHT  720
#define LCD_BIT_PER_PIXEL  16
#define PIN_NUM_LCD_RST  3
#define DELAY_TIME_MS  3000
#define LCD_MIPI_DSI_LANE_NUM  2
#define MIPI_DSI_PHY_PWR_LDO_CHAN  3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV  2500
#define DISPLAY_SWAP_XY  false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

/* Backlight */
#define DISPLAY_BACKLIGHT_PIN  52
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT  false

/* Buttons */
#define BOOT_BUTTON_GPIO  35
#define VOLUME_UP_BUTTON_GPIO   (-1)  /* GPIO_NUM_NC */
#define VOLUME_DOWN_BUTTON_GPIO (-1)  /* GPIO_NUM_NC */
#define POWER_BUTTON_PIN  (-1)        /* GPIO_NUM_NC */

/* BT audio UART */
#define BT_AUDIO_TX_PIN  26
#define BT_AUDIO_RX_PIN  27

/* NT26 4G modem UART */
#define NT26_RX_PIN   29
#define NT26_TX_PIN   28
#define NT26_MRDY_PIN 13
#define NT26_SRDY_PIN 4

/* USB charge status */
#define USB_CHG_STA_PIN  53

/* SDMMC (SD card) */
#define SDMMC_CLK_PIN  43
#define SDMMC_CMD_PIN  44
#define SDMMC_D0_PIN   39
#define SDMMC_D1_PIN   40
#define SDMMC_D2_PIN   41
#define SDMMC_D3_PIN   42

/* USB OTG */
#define USB_OTG_DM_PIN  24
#define USB_OTG_DP_PIN  25

/* Status LED GPIO — the metalio-claw-4 schematic does not expose a
 * dedicated status LED GPIO.  Set to -1 (NC) to select NoLed.  Override
 * with -DCONFIG_METALIO_STATUS_LED_GPIO=<n> when the LED pin is known. */
#ifndef CONFIG_METALIO_STATUS_LED_GPIO
#define CONFIG_METALIO_STATUS_LED_GPIO  (-1)
#endif

/* ================================================================== */
/* Backlight abstraction                                              */
/* ================================================================== */

class Backlight
{
public:
    virtual ~Backlight() = default;
    virtual void SetBrightness(uint8_t brightness, bool fade = true) = 0;
    virtual void RestoreBrightness() = 0;
    virtual uint8_t GetBrightness() const = 0;
};

/* Backlight defaults — match MetalioClaw4 main/boards/common/backlight.h */
#define BACKLIGHT_DEFAULT_PERCENT  75
#define BACKLIGHT_MIN_PERCENT      5
#define BACKLIGHT_FREQ_HZ          25000

/* PwmBacklight — drives the LCD backlight through the NuttX LEDC/PWM
 * driver registered as /dev/pwm0 by the board bringup (board_ledc_setup).
 * The reference uses ESP-IDF ledc_timer_config (25 kHz, 10-bit, GPIO 52);
 * here we issue PWMIOC_SETCHARACTERISTICS / PWMIOC_START on /dev/pwm0. */
class PwmBacklight : public Backlight
{
public:
    PwmBacklight(int gpio, int output_invert);
    virtual ~PwmBacklight();

    void SetBrightness(uint8_t brightness, bool fade = true) override;
    void RestoreBrightness() override;
    uint8_t GetBrightness() const override { return brightness_; }

private:
    int gpio_;
    int output_invert_;
    int fd_;
    uint32_t freq_hz_;
    bool started_;
    uint8_t brightness_ = 0;
};

/* ================================================================== */
/* MetalioClaw4Board singleton                                        */
/* ================================================================== */

class MetalioClaw4Board
{
public:
    MetalioClaw4Board();
    static MetalioClaw4Board &GetInstance();
    ~MetalioClaw4Board();

    /* Top-level initialization — calls the individual Initialize* methods.
     * Idempotent (guarded by initialized_). */
    void Initialize();

    /* Subsystem initialization — safe to call individually. */
    void InitializeDisplay();
    void InitializeAudio();
    void InitializeNetwork();
    void InitializeBacklight();
    void InitializeLed();

    /* Subsystem accessors (may return nullptr before init). */
    Display *GetDisplay() const { return display_; }
    Backlight *GetBacklight() const { return backlight_; }
    Led *GetLed() const { return led_; }

    /* Battery — reads from the BQ27220 driver via the board_shim C
     * accessors (metalio_board_get_battery). */
    bool GetBatteryLevel(int &level, bool &charging, bool &discharging);

    std::string GetBoardType() const { return "metalio-claw-4"; }
    std::string GetFirmwareVersion() const { return "1.0.0-openvela"; }

private:
    MetalioClaw4Board(const MetalioClaw4Board &) = delete;
    MetalioClaw4Board &operator=(const MetalioClaw4Board &) = delete;

    /* System monitor task — logs CPU / memory / battery every second.
     * Mirrors the monitoring task in the reference METALIO_CLAW_4 ctor. */
    void StartMonitorTask();

    Display *display_ = nullptr;
    Backlight *backlight_ = nullptr;
    Led *led_ = nullptr;
    bool initialized_{false};
    bool network_started_{false};
};

#endif /* METALIO_CLAW_4_BOARD_H */
