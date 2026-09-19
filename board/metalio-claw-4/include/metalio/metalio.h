#ifndef METALIO_METALIO_H
#define METALIO_METALIO_H

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* CX25601N standalone 1-cell buck charger (present on newer Claw4 boards) */

#define CX25601N_I2C_ADDR           0x6B

#define CX25601N_ICHG_MIN_MA        80
#define CX25601N_ICHG_MAX_MA        3040
#define CX25601N_ICHG_STEP_MA       80

/* IINDPM = code * 20 mA, code 5..150 -> 100..3000 mA */
#define CX25601N_IINDPM_MIN_MA      100
#define CX25601N_IINDPM_MAX_MA      3000
#define CX25601N_IINDPM_STEP_MA     20

#define CX25601N_VREG_MIN_MV        3840
#define CX25601N_VREG_MAX_MV        4800
#define CX25601N_VREG_STEP_MV       10

/* VOTG = code * 80 mV, code 0x30..0x42 -> 3840..5280 mV */
#define CX25601N_VOTG_MIN_MV        3840
#define CX25601N_VOTG_MAX_MV        5280
#define CX25601N_VOTG_STEP_MV       80

/* IOTG = code * 20 mA, code 5..60 -> 100..1200 mA */
#define CX25601N_IOTG_MIN_MA        100
#define CX25601N_IOTG_MAX_MA        1200
#define CX25601N_IOTG_STEP_MA       20

/* OTG default output: ~5.04 V / 1 A */
#define CX25601N_OTG_DEFAULT_MV     5040
#define CX25601N_OTG_DEFAULT_MA     1000

/* REG0x1E CHG_STAT[4:3] */
#define CX25601N_CHG_STAT_NOT       0
#define CX25601N_CHG_STAT_CC        1
#define CX25601N_CHG_STAT_CV        2
#define CX25601N_CHG_STAT_TOPOFF    3

#ifdef __cplusplus
extern "C" {
#endif

int metalio_tca9555_initialize(FAR struct i2c_master_s *i2c);
int metalio_tca9555_write_pin(int pin, bool level);
int metalio_tca9555_read_pin(int pin, FAR bool *level);
int metalio_pwr_shutdown_pulse(void);

/* PWR_KEY (TCA9555 P0.5, active-low) short-press / long-press callbacks */

typedef void (*metalio_pwr_key_cb_t)(void);

int metalio_pwr_key_init(metalio_pwr_key_cb_t short_cb,
                         metalio_pwr_key_cb_t long_cb);

int metalio_bq27220_initialize(FAR struct i2c_master_s *i2c);
int metalio_bq27220_read_soc(FAR int *percent);
int metalio_bq27220_read_voltage_mv(FAR int *mv);
int metalio_bq27220_read_current_ma(FAR int *ma);

int metalio_nu1680_initialize(FAR struct i2c_master_s *i2c);
int metalio_nu1680_probe(void);

int metalio_sc7a20h_initialize(FAR struct i2c_master_s *i2c);
int metalio_sc7a20h_read_mg(FAR int *ax, FAR int *ay, FAR int *az);
/* Cached WHO_AM_I from initialize; returns -ENODEV if never probed. */
int metalio_sc7a20h_whoami(FAR uint8_t *who);

int metalio_qmc6309_initialize(FAR struct i2c_master_s *i2c);
int metalio_qmc6309_read_raw(FAR int16_t *mx, FAR int16_t *my,
                             FAR int16_t *mz);
/* Cached CHIP_ID from initialize; returns -ENODEV if never probed. */
int metalio_qmc6309_chip_id(FAR uint8_t *id);
/* Read axes + STATUS reg 0x09 (DRDY/OVL). status may be NULL. */
int metalio_qmc6309_read_raw_ex(FAR int16_t *mx, FAR int16_t *my,
                                FAR int16_t *mz, FAR uint8_t *status);

int metalio_cx25601n_initialize(FAR struct i2c_master_s *i2c);
bool metalio_cx25601n_is_ready(void);
int metalio_cx25601n_read_reg(uint8_t reg, FAR uint8_t *val);
int metalio_cx25601n_enable_charge(bool enable);
int metalio_cx25601n_is_charge_enabled(FAR bool *enabled);
int metalio_cx25601n_enable_otg(bool enable);
int metalio_cx25601n_is_otg_enabled(FAR bool *enabled);
int metalio_cx25601n_set_ichg_ma(uint32_t ma);
int metalio_cx25601n_get_ichg_ma(FAR uint32_t *ma);
int metalio_cx25601n_set_iindpm_ma(uint32_t ma);
int metalio_cx25601n_get_iindpm_ma(FAR uint32_t *ma);
int metalio_cx25601n_set_vreg_mv(uint32_t mv);
int metalio_cx25601n_get_vreg_mv(FAR uint32_t *mv);
int metalio_cx25601n_get_chrg_stat(FAR uint8_t *stat);
int metalio_cx25601n_get_vbus_stat(FAR uint8_t *stat);
FAR const char *metalio_cx25601n_chrg_stat_str(uint8_t stat);
FAR const char *metalio_cx25601n_vbus_stat_str(uint8_t stat);

int metalio_gt911_initialize(FAR struct i2c_master_s *i2c);
int metalio_display_initialize(void);

/* ESP-Hosted Wi-Fi scan result.  `authmode` mirrors ESP-IDF
 * wifi_auth_mode_t (WIFI_AUTH_OPEN == 0 .. WIFI_AUTH_WPA2_WPA3_PSK == 6). */

#define METALIO_WIFI_MAX_AP 32

struct metalio_wifi_ap_s
{
  char ssid[33];
  int8_t rssi;
  int authmode;
};

int metalio_esp_hosted_initialize(void);
int metalio_esp_hosted_start_wifi(void);
int metalio_esp_hosted_connect(FAR const char *ssid, FAR const char *pass);
int metalio_esp_hosted_disconnect(void);
bool metalio_esp_hosted_is_connected(void);
FAR const char *metalio_esp_hosted_get_ssid(void);
int metalio_esp_hosted_scan(FAR struct metalio_wifi_ap_s *out, int max_ap);

/* Persist Settings across reboot (SPI flash sector @ 0x1F00000). */
int metalio_kvflash_read(FAR void *buf, size_t buflen);
int metalio_kvflash_write(FAR const void *buf, size_t len);

int metalio_sdcard_initialize(void);
int metalio_sdcard_mount_ro(void);
bool metalio_sdcard_is_mounted(void);
int metalio_gps_initialize(void);
int metalio_gps_get_snapshot(FAR int *fix_quality, FAR int *sats,
                             FAR double *lat, FAR double *lon);
int metalio_camera_initialize(FAR struct i2c_master_s *i2c);
int metalio_camera_power(bool on);
int metalio_camera_disable_test_pattern(void);
int metalio_bt_audio_initialize(void);
int metalio_bt_power(bool on);
int metalio_i2s_audio_initialize(void);
int metalio_nt26_initialize(void);


int metalio_dual_network_initialize(void);
int metalio_dual_network_select(int cellular);
int metalio_dual_network_path(void);
int metalio_ota_check(const char *url);
int metalio_standby_enter(void);

#ifdef __cplusplus
}
#endif
#endif
