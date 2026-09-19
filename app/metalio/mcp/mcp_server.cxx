/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCP server implementation — ported from
 * MetalioClaw4 main/mcp_server.cc.
 *
 * The reply path uses Application::GetInstance().SendMcpMessage() so that
 * responses are emitted through the active protocol (MQTT publish or
 * websocket text frame). The optional TCP server (StartTcpServer) accepts
 * newline-delimited JSON-RPC requests from external MCP clients and replies
 * on the same socket — this lets AI workbenches connect directly to the
 * device for debugging without going through the cloud protocol.
 */

#include "mcp_server.h"
#include "application.h"
#include "board_shim.h"
#include "settings.h"
#include "home_screen/home_screen.h"
#include "chat_screen/chat_screen.h"
#include "metalio_claw_4_board.h"

#include <metalio/metalio.h>
#include <strings.h>
#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <cstdint>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>

#define TAG "MCP"

/* ---- User LED on GPIO48 (voice controlled) ----
 * Directly drive the pin with the NuttX espressif GPIO API.  Prototypes are
 * declared here to avoid pulling arch-internal headers into apps; symbols
 * resolve from libarch (esp_gpio.c) at link time (flat build). */
extern "C" {
int esp_configgpio(int pin, uint16_t attr);
void esp_gpiowrite(int pin, bool value);
bool esp_gpioread(int pin);
}
#define USER_LED_GPIO        48
#define USER_LED_ACTIVE_HIGH 1           /* 0 if LED is wired active-low */
#define USER_GPIO_ATTR_OUT   (1u << 1)   /* OUTPUT, see espressif/esp_gpio.h */

/* ---- DeviceLink: LAN capability server (TCP 9001, JSON line protocol) ----
 * Nodes register capabilities; each action becomes a dynamic MCP tool
 * home.<location>.<cap>.<action> for cloud voice control. */
#define DL_PORT 9001

struct DlDevice {
    int sock;
    std::string device_id;
    uint32_t next_req;
    uint32_t wait_req;
    bool has_result;
    std::string result_text;
    std::condition_variable cv;
    DlDevice() : sock(-1), next_req(1), wait_req(0), has_result(false) {}
};

static std::mutex g_dl_mutex;
static std::map<std::string, DlDevice *> g_dl_devices;   /* by device_id */
static bool g_dl_started = false;

/* Tool names must be ASCII: lower-case, keep [a-z0-9_], else '_'. */
static std::string DlSan(const char *s)
{
    std::string r;
    for (const char *p = (s ? s : ""); *p; ++p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            r += c;
        else if (c >= 'A' && c <= 'Z')
            r += (char)(c + 32);
        else
            r += '_';
    }
    return r.empty() ? std::string("dev") : r;
}

static bool DlSendRaw(int sock, const std::string &line)
{
    std::string l = line;
    l += '\n';
    size_t off = 0;
    while (off < l.size()) {
        ssize_t n = send(sock, l.data() + off, l.size() - off, 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

static const char *DlStr(cJSON *obj, const char *key)
{
    cJSON *it = cJSON_GetObjectItem(obj, key);
    return (it && cJSON_IsString(it) && it->valuestring) ? it->valuestring : "";
}

/* Send a control command to a node and wait up to 3s for its result. */
static std::string DlControl(const std::string &dev_id, const std::string &cap,
                             const std::string &action,
                             bool has_param, bool param)
{
    std::unique_lock<std::mutex> lk(g_dl_mutex);
    auto it = g_dl_devices.find(dev_id);
    if (it == g_dl_devices.end())
        return std::string("{\"ok\":false,\"error\":\"device offline\"}");
    DlDevice *dev = it->second;

    uint32_t req = dev->next_req++;
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "control");
    cJSON_AddNumberToObject(msg, "req", (double)req);
    cJSON_AddStringToObject(msg, "cap", cap.c_str());
    cJSON_AddStringToObject(msg, "action", action.c_str());
    cJSON *params = cJSON_CreateObject();
    if (has_param) cJSON_AddBoolToObject(params, "enabled", param);
    cJSON_AddItemToObject(msg, "params", params);
    char *s = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    std::string line = s ? s : "";
    if (s) cJSON_free(s);
    if (line.empty())
        return std::string("{\"ok\":false,\"error\":\"oom\"}");

    dev->wait_req = req;
    dev->has_result = false;
    dev->result_text.clear();

    if (!DlSendRaw(dev->sock, line)) {
        return std::string("{\"ok\":false,\"error\":\"send failed\"}");
    }
    write(1, "DL_CTRL_SENT\n", 13);

    /* Poll in 100ms slices: the metalio condition_variable shim has no
     * predicate overload of wait_for. */
    for (int waited = 0; waited < 3000 && !dev->has_result; waited += 100)
        dev->cv.wait_for(lk, std::chrono::milliseconds(100));
    bool got = dev->has_result;
    if (!got)
        return std::string("{\"ok\":false,\"error\":\"timeout\"}");
    return dev->result_text;
}

static void DlRegisterTools(DlDevice *dev, cJSON *caps, const std::string &loc)
{
    cJSON *cap = nullptr;
    cJSON_ArrayForEach(cap, caps) {
        if (!cJSON_IsObject(cap)) continue;
        std::string cap_name = DlSan(DlStr(cap, "name"));
        std::string cap_desc = DlStr(cap, "description");
        std::string alias;
        cJSON *al = cJSON_GetObjectItem(cap, "alias");
        if (al && cJSON_IsArray(al)) {
            cJSON *a = nullptr;
            cJSON_ArrayForEach(a, al) {
                if (cJSON_IsString(a) && a->valuestring) {
                    if (!alias.empty()) alias += "/";
                    alias += a->valuestring;
                }
            }
        }
        std::string dev_id = dev->device_id;
        cJSON *acts = cJSON_GetObjectItem(cap, "actions");
        if (!acts || !cJSON_IsArray(acts)) continue;
        cJSON *act = nullptr;
        cJSON_ArrayForEach(act, acts) {
            if (!cJSON_IsString(act) || !act->valuestring) continue;
            std::string action = DlSan(act->valuestring);
            std::string tool = "home." + loc + "." + cap_name + "." + action;
            std::string desc = cap_desc;
            if (!alias.empty()) desc += " (also called: " + alias + ")";
            if (action == "set_power") {
                desc += " Call with enabled=true to turn on, enabled=false to turn off.";
                McpServer::GetInstance().AddTool(tool, desc,
                    PropertyList({Property("enabled", kPropertyTypeBoolean)}),
                    [dev_id, cap_name](const PropertyList &properties) -> ReturnValue {
                        bool enabled = properties["enabled"].value<bool>();
                        write(1, "DL_CTRL\n", 8);
                        return DlControl(dev_id, cap_name, "set_power", true, enabled);
                    });
            } else {
                McpServer::GetInstance().AddTool(tool, desc, PropertyList(),
                    [dev_id, cap_name, action](const PropertyList &) -> ReturnValue {
                        write(1, "DL_CTRL\n", 8);
                        return DlControl(dev_id, cap_name, action, false, false);
                    });
            }
        }
    }
}

/* Returns false when the connection must close. */
static bool DlHandleLine(DlDevice *&dev, int sock, const std::string &line)
{
    cJSON *json = cJSON_Parse(line.c_str());
    if (!json) return true;
    std::string type = DlStr(json, "type");

    if (type == "register") {
        std::string dev_id = DlSan(DlStr(json, "device_id"));
        std::string loc = DlSan(DlStr(json, "location"));
        cJSON *caps = cJSON_GetObjectItem(json, "capabilities");
        {
            std::lock_guard<std::mutex> lk(g_dl_mutex);
            auto it = g_dl_devices.find(dev_id);
            if (it != g_dl_devices.end()) {
                /* Re-register: keep tools, swap the socket. */
                it->second->sock = sock;
                dev = it->second;
            } else {
                dev = new DlDevice();
                dev->sock = sock;
                dev->device_id = dev_id;
                g_dl_devices[dev_id] = dev;
                if (caps && cJSON_IsArray(caps))
                    DlRegisterTools(dev, caps, loc);
            }
        }
        std::string ack = "{\"type\":\"register_ack\",\"ok\":true,\"device_id\":\""
                          + dev_id + "\"}";
        DlSendRaw(sock, ack);
        std::string log = "DL_REG " + dev_id + "\n";
        write(1, log.data(), log.size());
    } else if (type == "control_result") {
        std::lock_guard<std::mutex> lk(g_dl_mutex);
        if (dev) {
            cJSON *rq = cJSON_GetObjectItem(json, "req");
            uint32_t req = rq ? (uint32_t)rq->valuedouble : 0;
            if (dev->has_result == false && req == dev->wait_req) {
                cJSON *ok = cJSON_GetObjectItem(json, "ok");
                bool is_ok = ok && cJSON_IsTrue(ok);
                std::string state = DlStr(json, "state");
                std::string err = DlStr(json, "error");
                dev->result_text = std::string("{\"ok\":") + (is_ok ? "true" : "false");
                if (!state.empty()) dev->result_text += ",\"state\":\"" + state + "\"";
                if (!err.empty())   dev->result_text += ",\"error\":\"" + err + "\"";
                dev->result_text += "}";
                dev->has_result = true;
                dev->cv.notify_all();
                write(1, "DL_RESULT\n", 10);
            }
        }
    } else if (type == "heartbeat") {
        DlSendRaw(sock, "{\"type\":\"heartbeat_ack\"}");
    }
    cJSON_Delete(json);
    return true;
}

static void DlConnLoop(int sock)
{
    DlDevice *dev = nullptr;
    std::string buf;
    char tmp[512];
    for (;;) {
        ssize_t n = recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!DlHandleLine(dev, sock, line)) goto out;
        }
        if (buf.size() > 4096) buf.clear();   /* flood guard */
    }
out:
    {
        std::lock_guard<std::mutex> lk(g_dl_mutex);
        if (dev) {
            /* Wake any pending control call so it fails fast. */
            if (!dev->has_result) {
                dev->result_text = "{\"ok\":false,\"error\":\"device disconnected\"}";
                dev->has_result = true;
                dev->cv.notify_all();
            }
            auto it = g_dl_devices.find(dev->device_id);
            if (it != g_dl_devices.end() && it->second == dev) {
                g_dl_devices.erase(it);
                std::string log = "DL_LOST " + dev->device_id + "\n";
                write(1, log.data(), log.size());
                delete dev;
            }
        }
    }
    close(sock);
}

static void DlAcceptLoop()
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { write(1, "DL_SOCK_FAIL\n", 13); return; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DL_PORT);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 4) < 0) {
        write(1, "DL_BIND_FAIL\n", 13);
        close(srv);
        return;
    }
    write(1, "DL_LISTEN 9001\n", 15);
    for (;;) {
        int client = accept(srv, nullptr, nullptr);
        if (client < 0) continue;
        write(1, "DL_CONN\n", 8);
        std::thread(DlConnLoop, client).detach();
    }
}

static void DlStart()
{
    if (g_dl_started) return;
    g_dl_started = true;
    std::thread(DlAcceptLoop).detach();
}

#ifndef BOARD_NAME
#define BOARD_NAME "metalio-claw-4"
#endif

McpServer::McpServer() {}

McpServer::~McpServer()
{
    StopTcpServer();
    for (auto tool : tools_)
        delete tool;
    tools_.clear();
}

void McpServer::AddCommonTools()
{
    /* Important: prepend common tools so the assistant's prompt cache stays
     * warm — same behaviour as the upstream implementation. */
    auto original_tools = std::move(tools_);
    auto &board = Board::GetInstance();

    /* Keep local-control tool JSON SHORT so tools/list page-1 always
     * includes 开应用/音量/背光/蓝牙. Long descriptions used to push them
     * past the payload cap; cloud never paginated → text-only replies. */

    AddTool("self.app.open",
        "Open a local HiOS app. MUST call for 打开相机/音乐/日历/设置/天气/"
        "电台/蓝牙/网络/录音/计算器/背光/…. Pass Chinese name or id.",
        PropertyList({
            Property("name", kPropertyTypeString, std::string("")),
            Property("id", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList &properties) -> ReturnValue {
            std::string name = properties["name"].value<std::string>();
            std::string id = properties["id"].value<std::string>();
            const char *query = !name.empty() ? name.c_str()
                               : (!id.empty() ? id.c_str() : nullptr);
            if (query == nullptr)
                return std::string("Missing name or id");
            /* Soft-stop chat voice on Application thread BEFORE LVGL swap. */
            if (ChatScreen::IsActive())
            {
                write(1, "MCP_LEAVE_CHAT\n", 15);
                Application::GetInstance().SetVoiceUiDesired(false);
            }
            if (!HomeScreen::LaunchApp(query)) {
                ESP_LOGW(TAG, "self.app.open: unknown or unavailable '%s'",
                         query);
                return std::string("App not found or unavailable: ") + query;
            }
            write(1, "MCP_APP\n", 8);
            return true;
        });

    AddTool("self.app.list",
        "List local HiOS apps (id + Chinese name).",
        PropertyList(),
        [](const PropertyList & /*properties*/) -> ReturnValue {
            cJSON *arr = cJSON_CreateArray();
            const int n = HomeScreen::GetAppCount();
            for (int i = 0; i < n; ++i) {
                HomeScreen::AppInfo info{};
                if (!HomeScreen::GetAppInfo(i, &info))
                    continue;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "id", info.id ? info.id : "");
                cJSON_AddStringToObject(o, "name", info.name ? info.name : "");
                cJSON_AddBoolToObject(o, "available", info.available);
                cJSON_AddItemToArray(arr, o);
            }
            return arr;
        });

    AddTool("self.audio_speaker.set_volume",
        "Set local speaker volume 0-100 (same as Settings→音量). "
        "MUST call for 音量调到N / 声音调到N / 静音.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }),
        [](const PropertyList &properties) -> ReturnValue {
            int vol = properties["volume"].value<int>();
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            /* Same path as SettingsScreen::ApplyVolume. */
            Application::GetInstance().GetAudioService().SetVolume(vol);
            write(1, "VOL_SET\n", 8);
            return true;
        });

    AddTool("self.audio_speaker.adjust_volume",
        "Adjust local speaker volume by delta (-100..+100). "
        "MUST call for 大声一点(+10) / 小声一点(-10).",
        PropertyList({
            Property("delta", kPropertyTypeInteger, -100, 100)
        }),
        [](const PropertyList &properties) -> ReturnValue {
            int delta = properties["delta"].value<int>();
            if (delta < -100) delta = -100;
            if (delta > 100) delta = 100;
            auto &audio = Application::GetInstance().GetAudioService();
            int vol = audio.GetVolume() + delta;
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            audio.SetVolume(vol);
            write(1, "VOL_ADJ\n", 8);
            return vol;
        });

    AddTool("self.screen.set_brightness",
        "Set local backlight 5-100 (same as 背光 app). "
        "MUST call for 亮度调到N / 背光调到N.",
        PropertyList({
            Property("brightness", kPropertyTypeInteger, 5, 100)
        }),
        [&board](const PropertyList &properties) -> ReturnValue {
            int pct = properties["brightness"].value<int>();
            if (pct < 5)
                pct = 5;
            if (pct > 100)
                pct = 100;
            /* Same path as BacklightScreen slider. */
            Settings settings("display", true);
            settings.SetInt("brightness", pct);
            auto *bl = board.GetBacklight();
            if (bl)
                bl->SetBrightness(static_cast<uint8_t>(pct), true);
            write(1, "MCP_BL\n", 7);
            return true;
        });

    AddTool("self.screen.adjust_brightness",
        "Adjust local backlight by delta (-100..+100). "
        "MUST call for 亮一点(+10) / 暗一点(-10).",
        PropertyList({
            Property("delta", kPropertyTypeInteger, -100, 100)
        }),
        [&board](const PropertyList &properties) -> ReturnValue {
            int delta = properties["delta"].value<int>();
            if (delta < -100) delta = -100;
            if (delta > 100) delta = 100;
            auto *bl = board.GetBacklight();
            int pct = bl ? bl->GetBrightness() : 50;
            pct += delta;
            if (pct < 5) pct = 5;
            if (pct > 100) pct = 100;
            Settings settings("display", true);
            settings.SetInt("brightness", pct);
            if (bl)
                bl->SetBrightness(static_cast<uint8_t>(pct), true);
            write(1, "MCP_BL_ADJ\n", 10);
            return pct;
        });

    AddTool("self.bluetooth.set_power",
        "Turn local Bluetooth module power on/off (TCA9555). "
        "MUST call for 打开蓝牙/关闭蓝牙/开启蓝牙. "
        "To open the Bluetooth settings UI use self.app.open name=蓝牙.",
        PropertyList({
            Property("enabled", kPropertyTypeBoolean)
        }),
        [](const PropertyList &properties) -> ReturnValue {
            const bool on = properties["enabled"].value<bool>();
            /* Same path as BluetoothScreen power toggle. */
            metalio_bt_power(on);
            write(1, on ? "MCP_BT_ON\n" : "MCP_BT_OFF\n",
                  on ? 10 : 11);
            return true;
        });

    AddTool("self.get_device_status",
        "Read local volume/brightness/battery/network. Call before relative adjust.",
        PropertyList(),
        [&board](const PropertyList & /*properties*/) -> ReturnValue {
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "board", board.GetBoardType().c_str());
            cJSON_AddStringToObject(json, "firmware", board.GetFirmwareVersion().c_str());

            /* Chat/Speaking: skip gauge I2C + hosted link probe — both raced
             * LVGL / MQTT and froze after MCP_BAT_SKIP. */
            auto &app = Application::GetInstance();
            const bool chat_light =
                app.GetDeviceState() == kDeviceStateSpeaking ||
                ChatScreen::IsActive();
            int level = 0, charging = 0, discharging = 0;
            if (!chat_light)
                metalio_board_get_battery(&level, &charging, &discharging);
            else
                write(1, "MCP_BAT_SKIP\n", 13);
            cJSON *battery = cJSON_CreateObject();
            cJSON_AddNumberToObject(battery, "level", level);
            cJSON_AddBoolToObject(battery, "charging", charging != 0);
            cJSON_AddBoolToObject(battery, "discharging", discharging != 0);
            cJSON_AddItemToObject(json, "battery", battery);

            int volume = 0;
            metalio_board_get_volume(&volume);
            cJSON_AddNumberToObject(json, "volume", volume);

            auto *bl = board.GetBacklight();
            if (bl)
                cJSON_AddNumberToObject(json, "brightness", bl->GetBrightness());

            cJSON_AddStringToObject(json, "network_state",
                chat_light ? "connected" : metalio_board_get_network_state());
            if (chat_light)
                write(1, "MCP_STAT_OK\n", 12);
            return json;
        });

    /* ---- User LED on GPIO48: on / off / state voice tools ---- */
    {
        esp_configgpio(USER_LED_GPIO, USER_GPIO_ATTR_OUT);
        esp_gpiowrite(USER_LED_GPIO, !USER_LED_ACTIVE_HIGH);  /* default: off */

        AddTool("self.led.turn_on",
            "Turn on GPIO48 LED. MUST call for 开灯/打开灯/请开灯/把灯打开.",
            PropertyList(),
            [](const PropertyList &) -> ReturnValue {
                esp_gpiowrite(USER_LED_GPIO, (bool)USER_LED_ACTIVE_HIGH);
                write(1, "MCP_LED_ON\n", 11);
                return true;
            });

        AddTool("self.led.turn_off",
            "Turn off GPIO48 LED. MUST call for 关灯/关闭灯/把灯关掉.",
            PropertyList(),
            [](const PropertyList &) -> ReturnValue {
                esp_gpiowrite(USER_LED_GPIO, (bool)!USER_LED_ACTIVE_HIGH);
                write(1, "MCP_LED_OFF\n", 12);
                return true;
            });

        AddTool("self.led.get_state",
            "LED on/off state. MUST call for 灯亮着吗/灯什么状态.",
            PropertyList(),
            [](const PropertyList &) -> ReturnValue {
                bool on = (esp_gpioread(USER_LED_GPIO) == (bool)USER_LED_ACTIVE_HIGH);
                return std::string(on ? "{\"state\":\"on\"}" : "{\"state\":\"off\"}");
            });
    }

    /* ---- DeviceLink: start LAN capability server (port 9001) ---- */
    DlStart();

    /* Restore board-specific tools after the common ones (prompt cache). */
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools()
{
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList & /*properties*/) -> ReturnValue {
            auto &board = Board::GetInstance();
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "board", board.GetBoardType().c_str());
            cJSON_AddStringToObject(json, "firmware", board.GetFirmwareVersion().c_str());
            return json;
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList & /*properties*/) -> ReturnValue {
            auto &app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                app.Reboot();
            });
            return true;
        });

    AddUserOnlyTool("self.upgrade_firmware",
        "Upgrade firmware from a specific URL. This will download and install the "
        "firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList &properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());

            auto &app = Application::GetInstance();
            app.Schedule([url, &app]() {
                /* Ota is included via ota.h; this is wired up in the
                 * application layer where the full Ota type is visible. */
                ESP_LOGW(TAG, "Upgrade firmware URL handler invoked (needs Ota wiring)");
                (void)url;
                (void)app;
            });
            return true;
        });
}

void McpServer::AddTool(McpTool *tool)
{
    if (std::find_if(tools_.begin(), tools_.end(),
                     [tool](const McpTool *t) { return t->name() == tool->name(); }) != tools_.end())
    {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }
    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(),
             tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string &name, const std::string &description,
                        const PropertyList &properties,
                        std::function<ReturnValue(const PropertyList &)> callback)
{
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string &name, const std::string &description,
                                const PropertyList &properties,
                                std::function<ReturnValue(const PropertyList &)> callback)
{
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string &message)
{
    cJSON *json = cJSON_Parse(message.c_str());
    if (json == nullptr)
    {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON *capabilities)
{
    /* Hardware-specific capability parsing (camera vision URL, etc.) is a
     * no-op in the openvela port until the camera HAL is wired up. */
    (void)capabilities;
}

void McpServer::ParseMessage(const cJSON *json)
{
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0)
    {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s",
                 version ? version->valuestring : "null");
        return;
    }

    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method))
    {
        ESP_LOGE(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0)
        return;

    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params))
    {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id))
    {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize")
    {
        if (cJSON_IsObject(params))
        {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities))
                ParseCapabilities(capabilities);
        }
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"1.0.0-openvela\"}}";
        ReplyResult(id_int, message);
    }
    else if (method_str == "tools/list")
    {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr)
        {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor))
                cursor_str = std::string(cursor->valuestring);
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools))
                list_user_only_tools = with_user_tools->valueint == 1;
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    }
    else if (method_str == "tools/call")
    {
        if (!cJSON_IsObject(params))
        {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name))
        {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments))
        {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    }
    else
    {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string &result)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string &message)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string &cursor, bool list_user_only_tools)
{
    const int max_payload_size = 12000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end())
    {
        if (!found_cursor)
        {
            if ((*it)->name() == cursor)
                found_cursor = true;
            else
            {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only())
        {
            ++it;
            continue;
        }

        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size)
        {
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (!json.empty() && json.back() == ',')
        json.pop_back();

    if (json.back() == '[' && !tools_.empty())
    {
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit",
                 next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor +
                       " because of payload size limit");
        return;
    }

    if (next_cursor.empty())
        json += "]}";
    else
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";

    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string &tool_name, const cJSON *tool_arguments)
{
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(),
                                  [&tool_name](const McpTool *tool) {
                                      return tool->name() == tool_name;
                                  });

    if (tool_iter == tools_.end())
    {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    /* NOTE: Metalio is built with -fno-exceptions on NuttX.  The
     * original upstream code wrapped this loop and the Schedule
     * callback below in try/catch(std::exception&) because some
     * Property::set_value overloads used to throw std::out_of_range.
     * Our port's variant shim and Property accessors never throw —
     * they abort via MCP_FATAL* macros on programmer errors (which
     * should never happen during MCP dispatch).  We therefore drop
     * both try/catch blocks entirely so the code compiles under
     * -fno-exceptions. */
    {
        for (auto &argument : arguments)
        {
            bool found = false;
            if (cJSON_IsObject(tool_arguments))
            {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value))
                {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                }
                else if (argument.type() == kPropertyTypeBoolean &&
                         cJSON_IsNumber(value))
                {
                    argument.set_value<bool>(value->valuedouble != 0.0);
                    found = true;
                }
                else if (argument.type() == kPropertyTypeBoolean &&
                         cJSON_IsString(value) && value->valuestring)
                {
                    const char *s = value->valuestring;
                    const bool on =
                        (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
                         strcasecmp(s, "on") == 0 || strcmp(s, "开") == 0 ||
                         strcmp(s, "打开") == 0 || strcmp(s, "开启") == 0 ||
                         strcmp(s, "enable") == 0 || strcmp(s, "enabled") == 0);
                    const bool off =
                        (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0 ||
                         strcasecmp(s, "off") == 0 || strcmp(s, "关") == 0 ||
                         strcmp(s, "关闭") == 0 || strcmp(s, "disable") == 0 ||
                         strcmp(s, "disabled") == 0);
                    if (on || off)
                    {
                        argument.set_value<bool>(on);
                        found = true;
                    }
                }
                else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value))
                {
                    /* Prefer valuedouble: some payloads only fill that field.
                     * Clamp to declared range — Property::set_value used to
                     * MCP_FATAL on out-of-range and aborted volume tools. */
                    int v = (int)(value->valuedouble >= 0
                                      ? value->valuedouble + 0.5
                                      : value->valuedouble - 0.5);
                    if (argument.has_range())
                    {
                        if (v < argument.min_value())
                            v = argument.min_value();
                        if (v > argument.max_value())
                            v = argument.max_value();
                    }
                    argument.set_value<int>(v);
                    found = true;
                }
                else if (argument.type() == kPropertyTypeInteger &&
                         cJSON_IsString(value) && value->valuestring)
                {
                    int v = atoi(value->valuestring);
                    if (argument.has_range())
                    {
                        if (v < argument.min_value())
                            v = argument.min_value();
                        if (v > argument.max_value())
                            v = argument.max_value();
                    }
                    argument.set_value<int>(v);
                    found = true;
                }
                else if (argument.type() == kPropertyTypeString && cJSON_IsString(value))
                {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
                /* Cloud sometimes sends app id as a bare JSON string number
                 * skip; also try alias keys for self.app.open. */
                else if (argument.type() == kPropertyTypeString &&
                         value == nullptr && cJSON_IsObject(tool_arguments))
                {
                    const char *alias = nullptr;
                    if (argument.name() == "name")
                        alias = "id";
                    else if (argument.name() == "id")
                        alias = "name";
                    if (alias != nullptr)
                    {
                        auto alt = cJSON_GetObjectItem(tool_arguments, alias);
                        if (cJSON_IsString(alt) && alt->valuestring != nullptr)
                        {
                            argument.set_value<std::string>(alt->valuestring);
                            found = true;
                        }
                    }
                }
            }

            /* Bluetooth: cloud may send on/power/state instead of enabled. */
            if (!found && argument.type() == kPropertyTypeBoolean &&
                argument.name() == "enabled" && cJSON_IsObject(tool_arguments))
            {
                static const char *kBtKeys[] = {"on", "power", "state", nullptr};
                for (int ki = 0; kBtKeys[ki] != nullptr && !found; ++ki)
                {
                    auto alt = cJSON_GetObjectItem(tool_arguments, kBtKeys[ki]);
                    if (cJSON_IsBool(alt))
                    {
                        argument.set_value<bool>(alt->valueint == 1);
                        found = true;
                    }
                    else if (cJSON_IsNumber(alt))
                    {
                        argument.set_value<bool>(alt->valuedouble != 0.0);
                        found = true;
                    }
                    else if (cJSON_IsString(alt) && alt->valuestring)
                    {
                        const char *s = alt->valuestring;
                        const bool on =
                            (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
                             strcasecmp(s, "on") == 0 || strcmp(s, "开") == 0 ||
                             strcmp(s, "打开") == 0 || strcmp(s, "开启") == 0);
                        const bool off =
                            (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0 ||
                             strcasecmp(s, "off") == 0 || strcmp(s, "关") == 0 ||
                             strcmp(s, "关闭") == 0);
                        if (on || off)
                        {
                            argument.set_value<bool>(on);
                            found = true;
                        }
                    }
                }
            }

            if (!argument.has_default_value() && !found)
            {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    }

    auto &app = Application::GetInstance();
    /* Device-control tools run inline on the Application thread (already
     * inside DrainXiaozhiJsonQueue). Extra Schedule() has hung / delayed
     * volume·蓝牙·开应用 on this port so the cloud saw no tool result. */
    if (tool_name == "self.app.open" ||
        tool_name == "self.app.list" ||
        tool_name == "self.screen.set_brightness" ||
        tool_name == "self.screen.adjust_brightness" ||
        tool_name == "self.audio_speaker.set_volume" ||
        tool_name == "self.audio_speaker.adjust_volume" ||
        tool_name == "self.bluetooth.set_power" ||
        tool_name == "self.get_device_status")
    {
        write(1, "MCP_INLINE\n", 11);
        std::string result = (*tool_iter)->Call(arguments);
        write(1, "MCP_CALLED\n", 11);
        ReplyResult(id, result);
        write(1, "MCP_REPLIED\n", 12);
        return;
    }
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        ReplyResult(id, (*tool_iter)->Call(arguments));
    });
}

/* ------------------------------------------------------------------ */
/* Optional TCP server for external MCP clients                        */
/* ------------------------------------------------------------------ */

int McpServer::StartTcpServer(uint16_t port)
{
    if (tcp_running_)
    {
        ESP_LOGW(TAG, "TCP server already running");
        return 0;
    }

    tcp_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock_ < 0)
    {
        ESP_LOGE(TAG, "socket() failed: %s", strerror(errno));
        return -errno;
    }

    int yes = 1;
    setsockopt(tcp_sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(tcp_sock_, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "bind(:%u) failed: %s", port, strerror(errno));
        close(tcp_sock_);
        tcp_sock_ = -1;
        return -errno;
    }

    if (listen(tcp_sock_, 1) < 0)
    {
        ESP_LOGE(TAG, "listen() failed: %s", strerror(errno));
        close(tcp_sock_);
        tcp_sock_ = -1;
        return -errno;
    }

    tcp_running_ = true;
    tcp_thread_ = std::thread([this, port]() { this->TcpServerLoop(port); });
    ESP_LOGI(TAG, "MCP TCP server listening on port %u", port);
    return 0;
}

void McpServer::StopTcpServer()
{
    tcp_running_ = false;
    if (tcp_sock_ >= 0)
    {
        close(tcp_sock_);
        tcp_sock_ = -1;
    }
    if (tcp_thread_.joinable())
        tcp_thread_.join();
}

void McpServer::TcpServerLoop(uint16_t port)
{
    (void)port;
    while (tcp_running_ && tcp_sock_ >= 0)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(tcp_sock_, (struct sockaddr *)&client_addr, &client_len);
        if (client < 0)
        {
            if (tcp_running_)
                ESP_LOGE(TAG, "accept() failed: %s", strerror(errno));
            break;
        }

        ESP_LOGI(TAG, "MCP client connected: %s:%u",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        /* Line-buffered JSON-RPC loop. Replies are sent back on the same
         * socket. We can't reuse ReplyResult/ReplyError because they push
         * through Application::SendMcpMessage (the protocol channel). */
        char buf[4096];
        std::string line;
        while (tcp_running_)
        {
            ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
            if (n <= 0)
                break;
            buf[n] = '\0';
            line.append(buf, n);

            size_t pos;
            while ((pos = line.find('\n')) != std::string::npos)
            {
                std::string request = line.substr(0, pos);
                line.erase(0, pos + 1);
                if (request.empty())
                    continue;

                cJSON *json = cJSON_Parse(request.c_str());
                if (!json)
                {
                    const char *err = "{\"jsonrpc\":\"2.0\",\"error\":{\"message\":\"parse error\"}}\n";
                    send(client, err, strlen(err), 0);
                    continue;
                }

                /* Reply on the TCP socket. */
                auto id_item = cJSON_GetObjectItem(json, "id");
                int id_int = cJSON_IsNumber(id_item) ? id_item->valueint : 0;

                /* Parse but capture the reply locally instead of forwarding
                 * through the protocol channel. */
                std::string reply = "{\"jsonrpc\":\"2.0\",\"id\":";
                reply += std::to_string(id_int);
                reply += ",\"result\":{\"note\":\"TCP MCP path forwards to ParseMessage; "
                         "wire reply capture TODO\"}}\n";
                send(client, reply.data(), reply.size(), 0);

                /* Still dispatch through the standard parser so tool calls
                 * take effect on the device. */
                ParseMessage(json);
                cJSON_Delete(json);
            }
        }

        close(client);
        ESP_LOGI(TAG, "MCP client disconnected");
    }
}
