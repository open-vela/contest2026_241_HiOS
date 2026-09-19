/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MetalioClaw4Board implementation — ported from MetalioClaw4
 * main/boards/metalio-claw-4/metalio-claw-4.cc.
 *
 * On NuttX the low-level hardware bring-up (I2C bus, TCA9555 IO expander,
 * BQ27220 fuel gauge, NU1680 charger, GT911 touch, MIPI-DSI panel,
 * ESP-Hosted WiFi) is performed by the board library
 * (nuttx/boards/.../esp32p4_bringup.c) before the application starts.
 *
 * This class therefore focuses on the application-level wiring:
 *   - Constructing the Display (NuttxLvglDisplay — connects to /dev/fb0
 *     via the NuttX LVGL fbdev driver and creates the boot → home UI)
 *   - Starting the NetworkService (WiFi via esp-hosted)
 *   - Constructing the Backlight + LED drivers (stubs until NuttX
 *     GPIO/PWM userland drivers land)
 *   - Spawning the system-monitor task
 */

#include "metalio_claw_4_board.h"
#include "board_shim.h"
#include "network_service.h"
#include "gpio_led.h"
#include "nuttx_lvgl_display.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include "freertos_shim.h"
#include "pwr_key_handler/pwr_key_handler.h"
#include "settings.h"

#include <cstring>
#include <cstdlib>
#include <thread>
#include <time.h>

#if defined(CONFIG_NUTTX) || defined(__NUTTX__)
#  include <malloc.h>
#  include <syslog.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <metalio/metalio.h>   /* metalio_bq27220_read_soc / read_voltage_mv */
#include <arch/board/board.h>  /* BOARD_IOEXP_USB_INSERT / WIRELESS_CHG */
#include <nuttx/timers/pwm.h>  /* PWMIOC_* / struct pwm_info_s */

#define TAG "METALIO_CLAW_4"

/* ================================================================== */
/* PwmBacklight                                                       */
/*                                                                    */
/* Opens /dev/pwm0 (LEDC timer0, GPIO 52) registered by the board     */
/* bringup and drives the LCD backlight with a 25 kHz PWM signal.     */
/* ================================================================== */

PwmBacklight::PwmBacklight(int gpio, int output_invert)
    : gpio_(gpio), output_invert_(output_invert),
      fd_(-1), freq_hz_(BACKLIGHT_FREQ_HZ), started_(false)
{
    (void)gpio_;
    (void)output_invert_;

    fd_ = open("/dev/pwm0", O_WRONLY);
    if (fd_ < 0)
    {
        ESP_LOGW(TAG, "PwmBacklight: open /dev/pwm0 failed: %d", errno);
    }
}

PwmBacklight::~PwmBacklight()
{
    if (fd_ >= 0)
    {
        if (started_)
            ioctl(fd_, PWMIOC_STOP, 0);
        close(fd_);
        fd_ = -1;
    }
}

void PwmBacklight::SetBrightness(uint8_t brightness, bool fade)
{
    (void)fade;

    if (brightness > 100)
        brightness = 100;

    brightness_ = brightness;

    if (fd_ < 0)
    {
        ESP_LOGW(TAG, "PwmBacklight: no PWM device, brightness=%u ignored",
                 brightness);
        return;
    }

    /* duty is a ub16_t but the ESP32-P4 LEDC lower-half maps [0, UINT16_MAX]
     * onto the 10-bit hardware range (see ledc_duty_bin_conversion). */
    struct pwm_info_s info;
    memset(&info, 0, sizeof(info));
    info.frequency = freq_hz_;
    info.duty = (uint32_t)((brightness * 65535u) / 100u);
    info.cpol = PWM_CPOL_NDEF;
    info.dcpol = PWM_DCPOL_NDEF;

    if (ioctl(fd_, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info) < 0)
    {
        ESP_LOGW(TAG, "PwmBacklight: SETCHARACTERISTICS failed: %d", errno);
        return;
    }

    if (!started_)
    {
        if (ioctl(fd_, PWMIOC_START, 0) < 0)
        {
            ESP_LOGW(TAG, "PwmBacklight: START failed: %d", errno);
            return;
        }
        started_ = true;
    }
}

void PwmBacklight::RestoreBrightness()
{
    Settings settings("display", false);
    int v = settings.GetInt("brightness", BACKLIGHT_DEFAULT_PERCENT);
    if (v < BACKLIGHT_MIN_PERCENT)
        v = BACKLIGHT_MIN_PERCENT;
    if (v > 100)
        v = 100;
    SetBrightness(static_cast<uint8_t>(v), false);
}

/* ================================================================== */
/* MetalioClaw4Board                                                  */
/* ================================================================== */

/* Global instance — avoids __cxa_guard_acquire which uses
 * mutex_wrapper (pthread_mutex_init) that may not work in
 * early startup contexts on NuttX. */
static MetalioClaw4Board g_metalio_board;

MetalioClaw4Board &MetalioClaw4Board::GetInstance()
{
    return g_metalio_board;
}

MetalioClaw4Board::MetalioClaw4Board()
{
    /* Constructor minimal — heavy init in Initialize() */
}

MetalioClaw4Board::~MetalioClaw4Board()
{
    delete display_;
    delete backlight_;
    delete led_;
}

void MetalioClaw4Board::Initialize()
{
    if (initialized_)
        return;

    /* Order matters: backlight must exist before the display tries to
     * restore brightness, and the LED should be ready before the app
     * starts emitting state changes. */
    write(1, "BINIT: backlight\n", 17);
    InitializeBacklight();
    write(1, "BINIT: pwrkey\n", 14);
    /* Arm PWR_KEY before display init (matches MetalioClaw4 boot order) so
     * long-press shutdown works even while LCD/LVGL are still coming up. */
    PwrKey_Init();
    write(1, "BINIT: display\n", 15);
    InitializeDisplay();
    write(1, "BINIT: audio\n", 13);
    InitializeAudio();
    write(1, "BINIT: led\n", 11);
    InitializeLed();
    /* InitializeNetwork() deferred — NetworkService has C++ runtime
     * dependencies (std::function) that may hang during early startup.
     * Will be called later from Application::Start(). */

    write(1, "Board init done\n", 16);
    initialized_ = true;
}

/* ------------------------------------------------------------------ */
/* InitializeDisplay                                                  */
/*                                                                    */
/* The NuttX board bringup (metalio_display_initialize) already       */
/* configures the MIPI-DSI panel and registers /dev/fb0.  This        */
/* method constructs the application-level Display wrapper that       */
/* connects to it via NuttxLvglDisplay, which calls lv_init(),        */
/* creates the LVGL fbdev display, starts the event-loop thread,      */
/* and creates the boot → home screen UI.                             */
/* ------------------------------------------------------------------ */

void MetalioClaw4Board::InitializeDisplay()
{
    if (display_ != nullptr)
        return;

    display_ = new NuttxLvglDisplay();
    if (display_ == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create NuttxLvglDisplay");
        return;
    }

    /* Do NOT RestoreBrightness here. MetalioClaw4 turns the backlight on
     * only after LVGL+LCD are ready; lighting the panel before the first
     * boot frame is flushed shows a blank/garbage FB flash (闪屏).
     * Application::Start restores brightness after StartDisplayThread(). */
}

/* ------------------------------------------------------------------ */
/* InitializeAudio                                                    */
/*                                                                    */
/* The AudioService is owned by Application (audio_service_ member).  */
/* Application::Start() calls audio_service_.Initialize(nullptr) which*/
/* creates the default MetalioAudioCodec (direct I2S, no I2C codec).  */
/* This method just logs the audio configuration.                     */
/* The PA (power amplifier) enable is handled by the NuttX bringup's  */
/* TCA9555 IO-expander init (PA enabled after I2S init).              */
/* ------------------------------------------------------------------ */

void MetalioClaw4Board::InitializeAudio()
{
    /* AudioService + codec are owned by Application, created in Start().
     * PA (power amplifier) is enabled by NuttX bringup (TCA9555 P1.0). */
}

/* ------------------------------------------------------------------ */
/* InitializeNetwork                                                  */
/*                                                                    */
/* Starts the NetworkService which wraps the board-specific           */
/* metalio_esp_hosted_* (WiFi) and metalio_nt26_* (4G) helpers.       */
/* The reference uses DualNetworkBoard which auto-selects WiFi/4G;    */
/* on NuttX we default to WiFi (esp-hosted) with cached credentials.  */
/* ------------------------------------------------------------------ */

void MetalioClaw4Board::InitializeNetwork()
{
    if (network_started_)
    {
        return;
    }

    network_started_ = true;

#ifdef CONFIG_METALIO_ESP_HOSTED
    /* esp-hosted (WiFi) was already initialized in the NuttX board bringup
     * (esp32p4_bringup.c → metalio_esp_hosted_initialize), which registers
     * the eth0 netdev.  Bring up the STA link and obtain a DHCP lease using
     * UI-saved credentials when present, else Kconfig defaults (C5 non-split:
     * DHCP is host-side on eth0). */
    static NetworkService network;
    Settings wifi("wifi", false);
    std::string saved_ssid = wifi.GetString("ssid");
    std::string saved_pass = wifi.GetString("password");

    const char *ssid;
    const char *pass;
    if (!saved_ssid.empty())
    {
        ssid = saved_ssid.c_str();
        pass = saved_pass.empty() ? nullptr : saved_pass.c_str();
        ESP_LOGE(TAG, "InitializeNetwork: using saved WiFi '%s'", ssid);
    }
    else
    {
        ssid = CONFIG_METALIO_WIFI_SSID;
        pass = CONFIG_METALIO_WIFI_PASSWORD;
        if (pass != nullptr && pass[0] == '\0')
        {
            pass = nullptr;
        }
        if (ssid == nullptr || ssid[0] == '\0')
        {
            ssid = nullptr;
            pass = nullptr;
            ESP_LOGE(TAG, "InitializeNetwork: no saved/default SSID — WiFi up for scan only");
        }
    }

    int ret = network.StartWifi(ssid, pass);
    ESP_LOGI(TAG, "InitializeNetwork: StartWifi('%s') = %d, ip=%s",
             ssid ? ssid : "(null)", ret, network.GetIpAddress().c_str());
#endif
}

/* ------------------------------------------------------------------ */
/* InitializeBacklight                                                */
/* ------------------------------------------------------------------ */

void MetalioClaw4Board::InitializeBacklight()
{
    if (backlight_ != nullptr)
        return;

    backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN,
                                  DISPLAY_BACKLIGHT_OUTPUT_INVERT);
}

/* ------------------------------------------------------------------ */
/* InitializeLed                                                      */
/*                                                                    */
/* The metalio-claw-4 schematic does not expose a dedicated status    */
/* LED GPIO.  If CONFIG_METALIO_STATUS_LED_GPIO is set (>= 0), a      */
/* GpioLed is created on that pin; otherwise NoLed is used.           */
/* ------------------------------------------------------------------ */

void MetalioClaw4Board::InitializeLed()
{
    if (led_ != nullptr)
        return;

    int led_gpio = CONFIG_METALIO_STATUS_LED_GPIO;
    if (led_gpio >= 0)
    {
        led_ = new GpioLed(led_gpio);
    }
    else
    {
        led_ = new NoLed();
    }
}

/* ------------------------------------------------------------------ */
/* GetBatteryLevel                                                    */
/*                                                                    */
/* Reads from the BQ27220 fuel gauge via the board_shim C accessor.   */
/* The NuttX bringup initialises the BQ27220 driver (metalio_bq27220_ */
/* initialize) which populates the values that                       */
/* metalio_board_get_battery() reads.                                 */
/* ------------------------------------------------------------------ */

bool MetalioClaw4Board::GetBatteryLevel(int &level, bool &charging,
                                        bool &discharging)
{
    /* Read StateOfCharge() directly from the BQ27220 NuttX driver.
     * Do NOT route through metalio_board_get_battery() — that C shim
     * delegates back to this method and would recurse forever. */
    int soc = 0;
    if (metalio_bq27220_read_soc(&soc) != 0)
        return false;

    if (soc < 0)   soc = 0;
    if (soc > 100) soc = 100;
    level = soc;

    /* Claw4 gauge: Current() > +5 mA ⇒ charging.  At high SOC the pack
     * often sits in the ±5 mA dead-band while still on a charger — also
     * trust USB insert (active-high) and wireless DET.
     * Do NOT call metalio_nu1680_probe() here: I2C NACK when Qi is off
     * stalls the LVGL thread and can solid-blue home/standby. */
    int current_ma = 0;
    (void)metalio_bq27220_read_current_ma(&current_ma);
    const bool from_current = (current_ma > 5);

    bool usb = false;
    bool wchg = false;
    (void)metalio_tca9555_read_pin(BOARD_IOEXP_USB_INSERT, &usb);
    (void)metalio_tca9555_read_pin(BOARD_IOEXP_WIRELESS_CHG, &wchg);

    charging = from_current || usb || wchg;
    discharging = (current_ma < -5) && !charging;
    return true;
}

/* ------------------------------------------------------------------ */
/* StartMonitorTask                                                   */
/*                                                                    */
/* Mirrors the monitoring task in the reference METALIO_CLAW_4 ctor.  */
/* Logs CPU / memory / battery every second.  The ESP-IDF version     */
/* uses ulTaskGetIdleRunTimeCounterForCore for per-core CPU usage     */
/* and temperature_sensor_get_celsius for chip temperature; these     */
/* are stubbed on NuttX.                                              */
/* ------------------------------------------------------------------ */

void MetalioClaw4Board::StartMonitorTask()
{
    ESP_LOGI(TAG, "Starting system monitor task");

    std::thread([this]() {
        constexpr int kIntervalMs = 1000;
        while (true)
        {
            struct timespec ts;
            ts.tv_sec = kIntervalMs / 1000;
            ts.tv_nsec = (kIntervalMs % 1000) * 1000000L;
            nanosleep(&ts, nullptr);

            /* ---- Memory ---- */
#if defined(CONFIG_NUTTX) || defined(__NUTTX__)
            struct mallinfo mi = mallinfo();
            unsigned used_kb = (unsigned)(mi.uordblks / 1024);
            unsigned free_kb = (unsigned)(mi.fordblks / 1024);
            ESP_LOGI("系统监控", "@@@内存  | 已用: %6u KB | 空闲: %6u KB",
                     used_kb, free_kb);
#else
            ESP_LOGI("系统监控", "@@@内存  | (mallinfo unavailable)");
#endif

            /* ---- Battery ---- */
            int level = 0;
            bool charging = false, discharging = false;
            if (GetBatteryLevel(level, charging, discharging))
            {
                ESP_LOGI("系统监控",
                         "@@@电池  | 电量: %3d%% | 充电: %s | 放电: %s",
                         level, charging ? "是" : "否",
                         discharging ? "是" : "否");
            }
        }
    }).detach();
}
