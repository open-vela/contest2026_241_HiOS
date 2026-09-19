/****************************************************************************
 * boards/risc-v/esp32p4/metalio-claw-4/include/board.h
 *
 * Metalio Claw4 board definitions (ESP32-P4 host + ESP32-C5 Wi-Fi).
 * Pin map sourced from CloudZao/MetalioClaw4 main/boards/metalio-claw-4/config.h
 *
 ****************************************************************************/

#ifndef __BOARDS_RISCV_ESP32P4_METALIO_CLAW_4_INCLUDE_BOARD_H
#define __BOARDS_RISCV_ESP32P4_METALIO_CLAW_4_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO subsystem counts */

#define BOARD_NGPIOOUT    4
#define BOARD_NGPIOINT    2

/* I2C0 (GPIO 7/8): GT911, TCA9555, BQ27220, QMC6309, NU1680, OV2710 SCCB */

#define BOARD_I2C0_SDA_GPIO           7
#define BOARD_I2C0_SCL_GPIO           8

/* Display */

#define BOARD_DISPLAY_WIDTH           720
#define BOARD_DISPLAY_HEIGHT          720
#define BOARD_LCD_RST_GPIO            3
#define BOARD_LCD_BACKLIGHT_GPIO      52
#define BOARD_LCD_MIPI_DSI_LANES      2

/* Touch GT911 */

#define BOARD_TOUCH_GT911_ADDR_PRIMARY   0x5d
#define BOARD_TOUCH_GT911_ADDR_SECONDARY 0x14

/* Audio I2S (16 kHz) */

#define BOARD_I2S_MIC_WS_GPIO         10
#define BOARD_I2S_MIC_DIN_GPIO        11
#define BOARD_I2S_SPK_DOUT_GPIO       9
#define BOARD_I2S_SPK_BCLK_GPIO       12
#define BOARD_AUDIO_SAMPLE_RATE       16000

/* Bluetooth audio codec UART */

#define BOARD_BT_AUDIO_TX_GPIO        26
#define BOARD_BT_AUDIO_RX_GPIO        27

/* GPS UART */

#define BOARD_GPS_TX_GPIO             38
#define BOARD_GPS_RX_GPIO             37

/* NT26 4G UART + flow control */

#define BOARD_NT26_TX_GPIO            28
#define BOARD_NT26_RX_GPIO            29
#define BOARD_NT26_MRDY_GPIO          13
#define BOARD_NT26_SRDY_GPIO          4

/* microSD SDMMC slot0 4-bit */

#define BOARD_SDMMC_CLK_GPIO          43
#define BOARD_SDMMC_CMD_GPIO          44
#define BOARD_SDMMC_D0_GPIO           39
#define BOARD_SDMMC_D1_GPIO           40
#define BOARD_SDMMC_D2_GPIO           41
#define BOARD_SDMMC_D3_GPIO           42

/* ESP-Hosted SDIO (P4 host <-> C5 slave), Slot1 4-bit @ 40 MHz */

#define BOARD_HOSTED_SDIO_CMD_GPIO    50
#define BOARD_HOSTED_SDIO_CLK_GPIO    51
#define BOARD_HOSTED_SDIO_D0_GPIO     49
#define BOARD_HOSTED_SDIO_D1_GPIO     34
#define BOARD_HOSTED_SDIO_D2_GPIO     31
#define BOARD_HOSTED_SDIO_D3_GPIO     53
#define BOARD_HOSTED_SDIO_RESET_GPIO  54
#define BOARD_HOSTED_SDIO_RESET_ACTIVE_HIGH 1
#define BOARD_HOSTED_SDIO_CLOCK_KHZ   40000

/* Misc */

#define BOARD_BOOT_BUTTON_GPIO        35
#define BOARD_VIBRATION_GPIO          22
#define BOARD_USB_OTG_DM_GPIO         24
#define BOARD_USB_OTG_DP_GPIO         25
#define BOARD_CAM_XCLK_GPIO           32

/* TCA9555 IO expander @ 0x20 */

#define BOARD_TCA9555_ADDR            0x20
#define BOARD_IOEXP_GPS_POWER         0  /* P0.0 high-enable */
#define BOARD_IOEXP_PA_SWITCH         1  /* P0.1 */
#define BOARD_IOEXP_CAM_PWDN          2  /* P0.2 low-enable */
#define BOARD_IOEXP_SD_POWER          3  /* P0.3 low-enable */
#define BOARD_IOEXP_PWR_KEY_PULSE     4  /* P0.4 */
#define BOARD_IOEXP_PWR_KEY           5  /* P0.5 input */
#define BOARD_IOEXP_BT_POWER          6  /* P0.6 high-enable */
#define BOARD_IOEXP_RST_4G            7  /* P0.7 high-enable */
#define BOARD_IOEXP_PA_ENABLE         8  /* P1.0 high-enable */
#define BOARD_IOEXP_ACCEL_INT         9  /* P1.1 */
#define BOARD_IOEXP_USB_INSERT        10 /* P1.2 */
#define BOARD_IOEXP_WIRELESS_CHG      11 /* P1.3 */
#define BOARD_IOEXP_TOUCH_RST         14 /* P1.6 — GT911 reset, active-low */

/* I2C 7-bit addresses */

#define BOARD_I2C_ADDR_GT911          0x5d
#define BOARD_I2C_ADDR_TCA9555        0x20
#define BOARD_I2C_ADDR_BQ27220        0x55
#define BOARD_I2C_ADDR_NU1680         0x60
#define BOARD_I2C_ADDR_CX25601N       0x6b
#define BOARD_I2C_ADDR_QMC6309        0x7c
#define BOARD_I2C_ADDR_OV2710         0x36

#endif /* __BOARDS_RISCV_ESP32P4_METALIO_CLAW_4_INCLUDE_BOARD_H */
