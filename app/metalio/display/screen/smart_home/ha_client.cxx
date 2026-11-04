#include "hi_task_policy.h"
#include "ha_client.h"
#include "settings.h"
#include "ota.h"
#include "cJSON_compat.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <mutex>
#include <pthread.h>
#include <vector>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
struct Entry {
    HaDeviceState state;
    bool queued = false;
    bool level_command = false;
    int target = 0;
    Clock::time_point deadline;
    unsigned serial = 0;
};
std::mutex s_lock;
std::map<std::string, Entry> s_entries;
std::string s_url, s_token;
bool s_started = false, s_dirty = false;
bool s_devices_dirty = false;
std::string s_devices_blob;
unsigned s_generation = 0, s_serial = 0;

int Request(const std::string &url, const std::string &token,
            const std::string &path, const std::string &body, std::string *response)
{
    Http http;
    http.SetHeader("Authorization", "Bearer " + token);
    http.SetHeader("Content-Type", "application/json");
    if (!body.empty()) http.SetContent(body);
    if (!http.Open(body.empty() ? "GET" : "POST", url + path)) return kHaNetwork;
    const int status = http.GetStatusCode();
    if (status == 401 || status == 403) return kHaAuth;
    if (status == 404) return kHaMissing;
    if (status < 200 || status >= 300) return kHaResponse;
    if (response) {
        char buf[512];
        int n;
        while ((n = http.Read(buf, sizeof(buf))) > 0) {
            if (response->size() + n > 16384) return kHaResponse;
            response->append(buf, n);
        }
        if (n < 0) return kHaNetwork;
    }
    return kHaOk;
}

bool ParseState(const std::string &json, const std::string &entity, HaDeviceState *out)
{
    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) return false;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "entity_id");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const bool valid = cJSON_IsString(id) && entity == id->valuestring && cJSON_IsString(state);
    if (valid) {
        out->known = !std::strcmp(state->valuestring, "on") || !std::strcmp(state->valuestring, "off");
        out->available = out->known;
        out->on = !std::strcmp(state->valuestring, "on");
        out->dimmable = false;
        const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(root, "attributes");
        const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(attrs, "brightness");
        if (cJSON_IsNumber(brightness)) out->level = std::max(0, std::min(100, (brightness->valueint * 100 + 127) / 255));
        const cJSON *modes = cJSON_GetObjectItemCaseSensitive(attrs, "supported_color_modes");
        const cJSON *mode;
        cJSON_ArrayForEach(mode, modes) {
            if (cJSON_IsString(mode) && std::strcmp(mode->valuestring, "onoff") && std::strcmp(mode->valuestring, "unknown"))
                out->dimmable = true;
        }
        out->error = kHaOk;
    }
    cJSON_Delete(root);
    return valid;
}

void *Worker(void *)
{
    auto next_poll = Clock::now();
    for (;;) {
        std::string url, token;
        std::map<std::string, Entry> entries;
        unsigned generation;
        bool save, save_devices;
        std::string devices_blob;
        {
            std::lock_guard<std::mutex> guard(s_lock);
            url = s_url; token = s_token; entries = s_entries;
            generation = s_generation; save = s_dirty; s_dirty = false;
            save_devices = s_devices_dirty; s_devices_dirty = false;
            if (save_devices) devices_blob = s_devices_blob;
        }
        if (save) {
            Settings settings("homeassistant", true);
            settings.SetString("url", url);
            settings.SetString("token", token);
        }
        if (save_devices) {
            Settings settings("smarthome", true);
            settings.SetString("devices", devices_blob);
        }
        const bool poll = Clock::now() >= next_poll;
        if (poll) next_poll = Clock::now() + std::chrono::seconds(2);
        if (!url.empty() && !token.empty()) {
            for (const auto &item : entries) {
                const std::string &entity = item.first;
                Entry entry = item.second;
                if (!poll && !entry.queued && !entry.state.pending) continue;
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    auto it = s_entries.find(entity);
                    if (generation != s_generation || it == s_entries.end() || it->second.serial != entry.serial) continue;
                    it->second.queued = false;
                }
                int error = kHaOk;
                if (entry.queued) {
                    const bool light = entity.compare(0, 6, "light.") == 0;
                    std::string body = "{\"entity_id\":\"" + entity + "\"";
                    if (entry.level_command) body += ",\"brightness_pct\":" + std::to_string(entry.target);
                    body += "}";
                    std::string action = entry.level_command ? (entry.target ? "turn_on" : "turn_off") : (entry.target ? "turn_on" : "turn_off");
                    error = Request(url, token, "/api/services/" + std::string(light ? "light/" : "switch/") + action, body, nullptr);
                    std::printf("[smart-home] command entity=%s target=%d result=%d\n", entity.c_str(), entry.target, error);
                }
                HaDeviceState state = entry.state;
                if (error == kHaOk) {
                    std::string response;
                    error = Request(url, token, "/api/states/" + entity, "", &response);
                    if (error == kHaOk && !ParseState(response, entity, &state)) error = kHaResponse;
                }
                if (error != kHaOk) {
                    state.available = false;
                    state.known = false;
                    state.pending = false;
                    state.error = error;
                } else if (state.pending) {
                    const bool reached = state.known && (entry.level_command
                        ? (entry.target == 0 ? !state.on : state.on && std::abs(state.level - entry.target) <= 1)
                        : state.on == (entry.target != 0));
                    if (reached) {
                        state.pending = false;
                        std::printf("[smart-home] confirmed entity=%s on=%d level=%d\n", entity.c_str(), state.on ? 1 : 0, state.level);
                    }
                    else if (Clock::now() >= entry.deadline) {
                        state.pending = false;
                        state.error = kHaTimeout;
                    }
                }
                // Keep an execution failure visible until the next user command.
                if (error == kHaOk && !entry.state.pending && entry.state.error == kHaTimeout)
                    state.error = kHaTimeout;
                {
                    std::lock_guard<std::mutex> guard(s_lock);
                    auto it = s_entries.find(entity);
                    if (generation == s_generation && it != s_entries.end() && it->second.serial == entry.serial)
                        it->second.state = state;
                    if (generation == s_generation && (error == kHaNetwork || error == kHaAuth)) {
                        for (auto &other : s_entries) {
                            other.second.state.available = false;
                            other.second.state.known = false;
                            other.second.state.pending = false;
                            other.second.state.error = error;
                            other.second.queued = false;
                        }
                    }
                }
                if (error == kHaNetwork || error == kHaAuth) break;
            }
        }
        usleep(200000);
    }
    return nullptr;
}

bool Queue(const std::string &entity, int target, bool level)
{
    std::lock_guard<std::mutex> guard(s_lock);
    auto it = s_entries.find(entity);
    if (it == s_entries.end()) return false;
    Entry &entry = it->second;
    if (entry.state.pending && level) return false;
    if (s_url.empty() || s_token.empty()) { entry.state.error = kHaNotConfigured; return false; }
    if (!entry.state.available || !entry.state.known) return false;
    if (level && (!entry.state.dimmable || entity.compare(0, 6, "light."))) { entry.state.error = kHaUnsupported; return false; }
    entry.target = target;
    entry.level_command = level;
    entry.queued = true;
    entry.state.pending = true;
    entry.state.error = kHaOk;
    entry.deadline = Clock::now() + std::chrono::seconds(10);
    entry.serial = ++s_serial;
    return true;
}
} // namespace

bool HaClient_ValidEntity(const std::string &entity)
{
    if (entity.size() >= 96 || (entity.compare(0, 6, "light.") && entity.compare(0, 7, "switch."))) return false;
    const size_t dot = entity.find('.');
    if (dot + 1 >= entity.size()) return false;
    for (size_t i = dot + 1; i < entity.size(); ++i)
        if (!(entity[i] >= 'a' && entity[i] <= 'z') && !(entity[i] >= '0' && entity[i] <= '9') && entity[i] != '_') return false;
    return true;
}

void HaClient_Start()
{
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_started) return;
    Settings settings("homeassistant", false);
    s_url = settings.GetString("url", "");
    s_token = settings.GetString("token", "");
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 16384);
    s_started = hi_task_create_legacy(&thread, &attr, Worker, nullptr) == 0;
    pthread_attr_destroy(&attr);
}

std::string HaClient_Url() { std::lock_guard<std::mutex> guard(s_lock); return s_url; }
std::string HaClient_Token() { std::lock_guard<std::mutex> guard(s_lock); return s_token; }
bool HaClient_Configured() { std::lock_guard<std::mutex> guard(s_lock); return s_started && !s_url.empty() && !s_token.empty(); }

bool HaClient_Configure(const std::string &input, const std::string &token)
{
    // Existing Http TLS skips certificate verification: explicitly support LAN HTTP only.
    std::string url = input;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.compare(0, 7, "http://") || url.size() <= 7 || url.size() > 192) return false;
    if (url.find_first_of("\r\n\t @?#", 7) != std::string::npos || url.find('/', 7) != std::string::npos) return false;
    if (token.size() > 1024 || token.find_first_of("\r\n") != std::string::npos) return false;
    std::lock_guard<std::mutex> guard(s_lock);
    if (!s_started || (token.empty() && s_token.empty())) return false;
    s_url = url;
    if (!token.empty()) s_token = token;
    s_dirty = true;
    ++s_generation;
    for (auto &item : s_entries) { item.second = Entry{}; item.second.serial = ++s_serial; }
    return true;
}

bool HaClient_Bind(const std::string &entity, const std::string &replace)
{
    if (!HaClient_ValidEntity(entity)) return false;
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_entries.find(entity) == s_entries.end()) {
        if (s_entries.size() >= 12 && s_entries.find(replace) == s_entries.end()) return false;
        if (replace != entity) s_entries.erase(replace);
        Entry entry; entry.serial = ++s_serial; s_entries[entity] = entry;
    }
    return true;
}
void HaClient_Unbind(const std::string &entity) { std::lock_guard<std::mutex> guard(s_lock); s_entries.erase(entity); }
bool HaClient_State(const std::string &entity, HaDeviceState *out)
{
    std::lock_guard<std::mutex> guard(s_lock);
    auto it = s_entries.find(entity);
    if (!out || it == s_entries.end()) return false;
    *out = it->second.state;
    if (s_url.empty() || s_token.empty()) out->error = kHaNotConfigured;
    return true;
}
bool HaClient_Power(const std::string &entity, bool on) { return Queue(entity, on ? 1 : 0, false); }
bool HaClient_Level(const std::string &entity, int level) { return Queue(entity, std::max(0, std::min(100, level)), true); }
void HaClient_SaveDevices(const std::string &blob) {
    std::lock_guard<std::mutex> guard(s_lock);
    s_devices_blob = blob;
    s_devices_dirty = true;
}
