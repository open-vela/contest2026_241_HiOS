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

/* PWR_KEY (TCA9555 P0.5, active-low) callbacks.  press_cb runs on the
 * falling-edge (button down); short_cb / long_cb run on release timing. */

typedef void (*metalio_pwr_key_cb_t)(void);

int metalio_pwr_key_init(metalio_pwr_key_cb_t short_cb,
                         metalio_pwr_key_cb_t long_cb,
                         metalio_pwr_key_cb_t press_cb);

/* Boot power-on haptic. metalio_boot_claim_backlight_pwm() must succeed
 * before any /dev/pwm1 open: backlight and motor share one LEDC block,
 * and opening the motor first leaves the panel black. The backlight fd
 * stays open. metalio_boot_pwr_vibrate_once() then pulses the motor
 * once so a hardware power-on has tactile feedback; later power-key
 * presses do not buzz. */

int metalio_boot_claim_backlight_pwm(void);
void metalio_boot_release_backlight_pwm(void);
void metalio_boot_pwr_vibrate_once(void);

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
  uint8_t bssid[6];
  uint8_t channel;
};

/* Shared driver/application ABI: a mismatch corrupts scan arrays. */
#ifdef __cplusplus
static_assert(sizeof(metalio_wifi_ap_s) == 48 && offsetof(metalio_wifi_ap_s, channel) == 46,
              "WiFi scan ABI mismatch");
#else
_Static_assert(sizeof(struct metalio_wifi_ap_s) == 48 && offsetof(struct metalio_wifi_ap_s, channel) == 46,
               "WiFi scan ABI mismatch");
#endif

int metalio_esp_hosted_initialize(void);
int metalio_esp_hosted_start_wifi(void);
int metalio_esp_hosted_ensure_wifi_ready(void);
int metalio_esp_hosted_connect(FAR const char *ssid, FAR const char *pass);
int metalio_esp_hosted_disconnect(void);
int metalio_esp_hosted_set_radio(bool enabled);
bool metalio_esp_hosted_radio_enabled(void);
bool metalio_esp_hosted_is_connected(void);
FAR const char *metalio_esp_hosted_get_ssid(void);
int metalio_esp_hosted_scan(FAR struct metalio_wifi_ap_s *out, int max_ap);

/* Persist Settings across reboot (SPI flash sector @ 0x1F00000). */
int metalio_kvflash_read(FAR void *buf, size_t buflen);
int metalio_kvflash_write(FAR const void *buf, size_t len);

/* Sizes measured on this board at boot. Reading them does not touch flash. */
uint32_t metalio_flash_bytes(void);
size_t metalio_psram_bytes(void);
size_t metalio_sram_bytes(void);

int metalio_sdcard_initialize(void);
int metalio_sdcard_probe(void);
int metalio_sdcard_mount_ro(void);
int metalio_sdcard_capacity(uint64_t *total,uint64_t *available);
int metalio_sdcard_ensure_rw(void);
int metalio_sdcard_export_begin(uint32_t *blocks);
int metalio_sdcard_export_io(uint32_t sector, void *buffer, uint32_t count, bool write_data);
int metalio_sdcard_export_end(void);
int metalio_usb_msc_phy(bool enable);
void metalio_usb_msc_wait_event(void);
bool metalio_sdcard_is_mounted(void);
int metalio_gps_initialize(void);
struct metalio_gps_snapshot_s {
  int fix_quality, sats, satellites_view;
  bool position_valid, altitude_valid, speed_valid;
  double latitude, longitude, altitude_m, hdop, speed_kmh;
  char utc_time[24];
};
int metalio_gps_get_snapshot_ex(FAR struct metalio_gps_snapshot_s *snapshot);
int metalio_gps_get_snapshot(FAR int *fix_quality, FAR int *sats,
                             FAR double *lat, FAR double *lon);
int metalio_camera_initialize(FAR struct i2c_master_s *i2c);
int metalio_camera_ensure_ready(void);
int metalio_camera_power(bool on);
int metalio_camera_disable_test_pattern(void);
int metalio_bt_audio_initialize(void);
int metalio_bt_ensure_mode1(void);
int metalio_bt_accept_headset_stream(void);
bool metalio_bt_voice_ready(void);
void metalio_bt_leave_voice(void);
int metalio_bt_enter_music(void);
void metalio_bt_probe_clocks(void);
int metalio_bt_power(bool on);
bool metalio_bt_is_powered(void);
int metalio_bt_transport_trylock(void);
void metalio_bt_transport_unlock(void);
int metalio_i2s_audio_initialize(void);
int metalio_nt26_initialize(void);
int metalio_nt26_set_radio(bool enabled);
bool metalio_nt26_radio_enabled(void);
int metalio_app_apply_radios(int wifi, int cellular);
/* Per-card configuration. An empty APN requests the subscription default. */
#define METALIO_SIM_ID_MAX 24
#define METALIO_APN_MAX 101
#define METALIO_APN_USER_MAX 65
struct metalio_sim_profile_s {
  char iccid[METALIO_SIM_ID_MAX];
  char apn[METALIO_APN_MAX];
  char username[METALIO_APN_USER_MAX];
  char password[METALIO_APN_USER_MAX];
  int auth; /* 0 none, 1 PAP, 2 CHAP, 3 PAP/CHAP */
  int slot;
};
enum metalio_sim_status_e {
  METALIO_SIM_UNKNOWN, METALIO_SIM_READY, METALIO_SIM_ABSENT,
  METALIO_SIM_PIN, METALIO_SIM_SEARCHING, METALIO_SIM_DENIED,
  METALIO_SIM_REGISTERED
};
int metalio_nt26_get_sim_profile(struct metalio_sim_profile_s *profile);
int metalio_nt26_read_sim_profile(struct metalio_sim_profile_s *profile);
int metalio_app_read_sim_profile(struct metalio_sim_profile_s *profile);
int metalio_nt26_set_sim_profile(const struct metalio_sim_profile_s *profile);
int metalio_nt26_get_sim_status(void);
int metalio_app_load_sim_profile(const char *iccid, struct metalio_sim_profile_s *profile);
int metalio_app_save_sim_profile(const struct metalio_sim_profile_s *profile);
int metalio_app_configure_sim_profile(const struct metalio_sim_profile_s *profile);
/* Verified PINs are remembered per ICCID; READY cards receive no PIN write. */
int metalio_nt26_unlock_sim(const char *pin, int expected_slot);
int metalio_app_unlock_sim(const char *pin, int expected_slot);
int metalio_app_sim_pin_read(const char *id, char *pin, size_t size);
int metalio_app_sim_pin_remember(const char *id, const char *pin);
void metalio_app_sim_pin_forget(const char *id);
void metalio_bt_set_scan_mode(bool enabled);

void metalio_app_cellular_sim_changed(bool present);

int metalio_nt26_get_sim_slot(void);
int metalio_nt26_set_sim_slot(int slot);
int metalio_nt26_get_dns_servers(uint32_t servers[2]);
int metalio_nt26_get_signal_strength(void);
int metalio_nt26_get_lte_rsrp_dbm(void);
int metalio_nt26_location_query(const char *command, char *response, size_t response_size);
enum metalio_call_state_e {
  METALIO_CALL_IDLE, METALIO_CALL_INCOMING, METALIO_CALL_DIALING,
  METALIO_CALL_ACTIVE, METALIO_CALL_HELD
};
unsigned int metalio_nt26_sms_revision(void);
int metalio_nt26_sms_list(const char *storage, char *out, size_t capacity);
int metalio_nt26_sms_send_pdu(const char *hex, size_t tpdu_bytes);
/* Same submission semantics; reports numeric +CMS ERROR, or -1 if absent. */
int metalio_nt26_sms_send_pdu_ex(const char *hex, size_t tpdu_bytes,
                                int *cms_error);
int metalio_nt26_call_answer(void);
int metalio_nt26_call_poll(void);
int metalio_nt26_get_call_state(void);
void metalio_app_call_state_changed(int state);
void metalio_app_call_number_changed(const char *number);
int metalio_app_call_session(bool enabled);
int metalio_app_call_dial(const char *number);
int metalio_app_call_answer(void);
int metalio_app_call_hangup(void);
int metalio_app_call_poll(void);

int metalio_nt26_call_dial(const char *number);
int metalio_nt26_call_hangup(void);
void metalio_nt26_call_request_hangup(void);
void metalio_nt26_call_begin_dial(void);
/* Modem route owner: true selects NT26 (P0.1=0), false releases to
 * local audio (P0.1=1). Local players must use the guarded API below. */
int metalio_nt26_set_call_audio_path(bool enabled);
/* Keep NT26 selected; change only the local speaker output. */
int metalio_nt26_set_call_output(bool speaker);
/* Select P0.1=1 unless dial/answer/active call owns the analog route.
 * An incoming call may use it for its local ringtone before answering. */
int metalio_audio_select_local(void);
/* Controls: 1=mute, 2=DTMF, 3=PA enable, 4=set call volume 0..100,
 * 5=read call volume. Controls 4/5 return the confirmed level or -errno. */
int metalio_nt26_call_control(int control, int value);


int metalio_dual_network_initialize(void);
int metalio_dual_network_select(int cellular);
int metalio_dual_network_path(void);
int metalio_ota_check(const char *url);
int metalio_standby_enter(void);

#ifdef __cplusplus
}
#endif
#endif
