/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board abstraction shim implementation for NuttX/openvela.
 *
 * Delegates all subsystem initialization and accessors to
 * MetalioClaw4Board (boards/metalio_claw_4_board.cxx).  The Board
 * singleton remains the public face expected by Application and the
 * ported xiaozhi-esp32 code; MetalioClaw4Board does the real work.
 */

#include "board_shim.h"
#include "metalio_claw_4_board.h"
#include "esp_log_shim.h"
#include "esp_err_shim.h"
#include <metalio/metalio.h>
#include "application.h"
#include "font_awesome.h"
#include "settings.h"
#include "system_info.h"
#include "display.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <cstdlib>

#ifdef CONFIG_NET
#include <netutils/netlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static const char *TAG = "board_shim";

/* Global Board instance — avoids __cxa_guard_acquire which may hang
 * during early startup on NuttX.  Allocated lazily via placement-new
 * to keep the Board constructor private. */
static char g_board_storage[sizeof(Board)] alignas(Board);
static Board *g_board_ptr = nullptr;

Board &Board::GetInstance()
{
    if (!g_board_ptr)
    {
        g_board_ptr = new (g_board_storage) Board();
    }
    return *g_board_ptr;
}

void Board::Initialize()
{
    MetalioClaw4Board::GetInstance().Initialize();
}

void Board::InitializeDisplay()
{
    MetalioClaw4Board::GetInstance().InitializeDisplay();
}

void Board::InitializeAudio()
{
    MetalioClaw4Board::GetInstance().InitializeAudio();
}

void Board::InitializeNetwork()
{
    MetalioClaw4Board::GetInstance().InitializeNetwork();
}

Display *Board::GetDisplay()
{
    return MetalioClaw4Board::GetInstance().GetDisplay();
}

LcdDisplay *Board::GetLcd()
{
    /* The port does not yet expose an LcdDisplay subclass; GetDisplay()
     * returns the generic Display* which callers can use directly. */
    return nullptr;
}

AudioService *Board::GetAudioService()
{
    return &Application::GetInstance().GetAudioService();
}

Backlight *Board::GetBacklight()
{
    return MetalioClaw4Board::GetInstance().GetBacklight();
}

Led *Board::GetLed()
{
    return MetalioClaw4Board::GetInstance().GetLed();
}

std::string Board::GetBoardType() const
{
    return MetalioClaw4Board::GetInstance().GetBoardType();
}

std::string Board::GetFirmwareVersion() const
{
    return MetalioClaw4Board::GetInstance().GetFirmwareVersion();
}

std::string Board::GenerateUuid()
{
    uint8_t uuid[16] = {};
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0)
    {
        ssize_t n = read(fd, uuid, sizeof(uuid));
        close(fd);
        if (n != static_cast<ssize_t>(sizeof(uuid)))
            fd = -1;
    }
    if (fd < 0)
    {
        unsigned seed = static_cast<unsigned>(time(nullptr) ^ (uintptr_t)uuid);
        srand(seed);
        for (size_t i = 0; i < sizeof(uuid); ++i)
            uuid[i] = static_cast<uint8_t>(rand() & 0xff);
    }
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;

    char uuid_str[37];
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11],
             uuid[12], uuid[13], uuid[14], uuid[15]);
    return std::string(uuid_str);
}

void Board::EnsureUuid()
{
    if (!uuid_.empty())
        return;
    Settings settings("board", true);
    uuid_ = settings.GetString("uuid");
    if (uuid_.empty())
    {
        uuid_ = GenerateUuid();
        settings.SetString("uuid", uuid_);
        ESP_LOGI(TAG, "Generated device UUID=%s", uuid_.c_str());
    }
    else
    {
        ESP_LOGI(TAG, "Device UUID=%s", uuid_.c_str());
    }
}

std::string Board::GetUuid()
{
    EnsureUuid();
    return uuid_;
}

std::string Board::GetBoardJson()
{
    std::string json = "{";
    json += R"("type":")" BOARD_TYPE R"(",)";
    json += R"("name":")" BOARD_NAME R"(",)";
#ifdef CONFIG_NET
    struct in_addr addr;
    memset(&addr, 0, sizeof(addr));
    if (netlib_get_ipv4addr("eth0", &addr) == 0 && addr.s_addr != 0)
    {
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr, ip, sizeof(ip));
        json += R"("ip":")" + std::string(ip) + R"(",)";
    }
#endif
    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    json += "}";
    return json;
}

std::string Board::GetSystemInfoJson()
{
    EnsureUuid();
    std::string lang = I18n::GetLocaleCode() ? I18n::GetLocaleCode() : "zh-CN";
    std::string json = R"({"version":2,"language":")" + lang + R"(",)";
    json += R"("flash_size":)" + std::to_string(SystemInfo::GetFlashSize()) + ",";
    json += R"("minimum_free_heap_size":)" +
            std::to_string(SystemInfo::GetMinimumFreeHeapSize()) + ",";
    json += R"("mac_address":")" + SystemInfo::GetMacAddress() + R"(",)";
    json += R"("uuid":")" + uuid_ + R"(",)";
    json += R"("chip_model_name":")" + SystemInfo::GetChipModelName() + R"(",)";
    json += R"("chip_info":{"model":18,"cores":2,"revision":0,"features":0},)";
    json += R"("application":{)";
    json += R"("name":")" BOARD_NAME R"(",)";
    json += R"("version":")" + GetFirmwareVersion() + R"(",)";
    json += R"("compile_time":")" + std::string(__DATE__) + "T" +
            std::string(__TIME__) + R"(Z",)";
    json += R"("idf_version":"openvela",)";
    json += R"("elf_sha256":""})";
    json += ",";
    json += R"("display":{"monochrome":false,"width":)" +
            std::to_string(GetDisplay() ? GetDisplay()->width() : 720) +
            R"(,"height":)" +
            std::to_string(GetDisplay() ? GetDisplay()->height() : 720) + "}";
    json += ",";
    json += R"("board":)" + GetBoardJson();
    json += "}";
    return json;
}

extern "C" void board_shim_initialize(void)
{
    /* board_shim_initialize() — no logging to preserve USB serial buffer */
    Board::GetInstance().Initialize();
}

/* C-linkage hardware accessors — read from NuttX drivers via
 * MetalioClaw4Board.  Until the BQ27220 / network / audio drivers are
 * fully wired, these return safe defaults. */
extern "C" int metalio_board_get_battery(int *level, int *charging, int *discharging)
{
    int lvl = 0;
    bool chg = false, dis = false;
    bool ok = MetalioClaw4Board::GetInstance().GetBatteryLevel(lvl, chg, dis);
    if (level) *level = lvl;
    if (charging) *charging = chg ? 1 : 0;
    if (discharging) *discharging = dis ? 1 : 0;
    return ok ? 1 : 0;
}

extern "C" const char *metalio_board_get_network_icon(void)
{
    return metalio_esp_hosted_is_connected() ? FONT_AWESOME_WIFI
                                             : FONT_AWESOME_WIFI_SLASH;
}

extern "C" int metalio_board_get_volume(int *volume)
{
    if (volume)
        *volume = Application::GetInstance().GetAudioService().GetVolume();
    return 1;
}

extern "C" const char *metalio_board_get_network_state(void)
{
    return metalio_esp_hosted_is_connected() ? "connected" : "disconnected";
}
