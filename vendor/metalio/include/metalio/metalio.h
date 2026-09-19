#ifndef METALIO_METALIO_H
#define METALIO_METALIO_H

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

int metalio_kvflash_read(FAR void *buf, size_t buflen);
int metalio_kvflash_write(FAR const void *buf, size_t len);

int metalio_sdcard_initialize(void);
int metalio_sdcard_mount_ro(void);
bool metalio_sdcard_is_mounted(void);
int metalio_gps_initialize(void);
int metalio_gps_get_snapshot(FAR int *fix_quality, FAR int *sats,
                             FAR double *lat, FAR double *lon);
int metalio_bt_audio_initialize(void);
int metalio_bt_power(bool on);
int metalio_i2s_audio_initialize(void);
int metalio_nt26_initialize(void);
int metalio_camera_initialize(void);
int metalio_camera_power(bool on);


int metalio_dual_network_initialize(void);
int metalio_dual_network_select(int cellular);
int metalio_dual_network_path(void);
int metalio_ota_check(const char *url);
int metalio_standby_enter(void);

#ifdef __cplusplus
}
#endif
#endif
