/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX-compatible Settings implementation.
 * Replaces ESP-IDF NVS with a process-wide RAM key-value store, optionally
 * persisted to a writable filesystem (/sdcard when MetalioClaw4-style RW
 * FAT is mounted, else /tmp or /var).
 *
 * OTA mqtt/websocket values must still be readable later in the same boot
 * (MqttProtocol::Start), so RAM is the source of truth even when persist
 * fails.
 */

#include "settings.h"
#include "esp_log_shim.h"
#include "hi_task_policy.h"

#include <metalio/metalio.h>

extern "C" int metalio_kvflash_read(void *buf, size_t buflen);
extern "C" int metalio_kvflash_write(const void *buf, size_t len);

#include <cstdio>
#include <stdio.h>      /* ::remove() */
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <map>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <utility>

static const char *TAG = "settings";

struct NsStore
{
    bool loaded = false;
    std::map<std::string, std::string> kv;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, NsStore> g_store;
static bool g_persist_dir_resolved = false;
static bool g_persist_warned = false;
static bool g_use_kvflash = false;
static bool g_kvflash_loaded = false;
static std::string g_persisted_blob;
static char g_persist_dir[64];
static pthread_t g_ui_thread;
static bool g_ui_known = false;
static bool g_flush_needed = false;
static bool g_flush_queued = false;

void SettingsBindUiThread()
{
    g_ui_thread = pthread_self();
    g_ui_known = true;
}

bool SettingsOnUiThread()
{
    return g_ui_known && pthread_equal(pthread_self(), g_ui_thread);
}

static bool OnUiThread()
{
    return SettingsOnUiThread();
}

/****************************************************************************/

static std::string SerializeAllLocked()
{
    std::string out;
    out.reserve(1024);
    for (const auto &ns_it : g_store)
    {
        out += '@';
        out += ns_it.first;
        out += '\n';
        for (const auto &kv : ns_it.second.kv)
        {
            out += kv.first;
            out += '=';
            out += kv.second;
            out += '\n';
        }
    }
    return out;
}

static void DeserializeAllLocked(const char *data, int len)
{
    if (data == nullptr || len <= 0)
    {
        return;
    }

    std::string cur_ns;
    const char *p = data;
    const char *end = data + len;
    while (p < end)
    {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        std::string line(p, line_len);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        p = nl ? nl + 1 : end;

        if (line.empty())
        {
            continue;
        }
        if (line[0] == '@')
        {
            cur_ns = line.substr(1);
            NsStore &ns = g_store[cur_ns];
            ns.loaded = true;
            continue;
        }
        if (cur_ns.empty())
        {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            continue;
        }
        g_store[cur_ns].kv[line.substr(0, eq)] = line.substr(eq + 1);
        g_store[cur_ns].loaded = true;
    }
}

enum SaveAction { kSaveDone = 0, kSaveFail = 1, kSaveDefer = 2 };

static void LoadKvFlashLocked();

static void *FlushSettings(void *)
{
    int fails = 0;
    for (;;)
    {
        /* A new board writes its journal the first time an app opens.
         * Erasing during that screen's first frame latches the panel
         * blue. Wait until the transition has finished painting. */
        usleep(800000);
        pthread_mutex_lock(&g_lock);
        if (!g_kvflash_loaded)
            LoadKvFlashLocked();
        std::string blob = SerializeAllLocked();
        if (!g_use_kvflash || blob == g_persisted_blob || blob.size() > 4000)
        {
            g_flush_needed = false;
            g_flush_queued = false;
            pthread_mutex_unlock(&g_lock);
            return nullptr;
        }
        pthread_mutex_unlock(&g_lock);

        int rc = metalio_kvflash_write(blob.data(), blob.size());

        pthread_mutex_lock(&g_lock);
        if (rc < 0)
        {
            ESP_LOGE(TAG, "kvflash write failed: %d", rc);
            if (++fails < 4)
            {
                pthread_mutex_unlock(&g_lock);
                continue;
            }
            g_flush_queued = false;
            pthread_mutex_unlock(&g_lock);
            return nullptr;
        }
        fails = 0;
        if (SerializeAllLocked() == blob)
        {
            g_persisted_blob = std::move(blob);
            write(1, "KVFLASH_OK\n", 11);
        }
        pthread_mutex_unlock(&g_lock);
    }
}

static void KickFlush()
{
    pthread_mutex_lock(&g_lock);
    const bool start = g_flush_needed && !g_flush_queued;
    if (start)
        g_flush_queued = true;
    pthread_mutex_unlock(&g_lock);
    if (!start)
        return;
    pthread_t thread{};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 24576);
    /* Do not inherit the interface thread's SCHED_FIFO priority. */
    struct sched_param sp{};
    sp.sched_priority = 80;
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    if (hi_task_create_legacy(&thread, &attr, FlushSettings, nullptr) != 0)
    {
        pthread_mutex_lock(&g_lock);
        g_flush_queued = false;
        pthread_mutex_unlock(&g_lock);
    }
    pthread_attr_destroy(&attr);
}

static SaveAction SaveKvFlashLocked()
{
    std::string blob = SerializeAllLocked();
    // Reapplying an unchanged toggle/volume must not erase flash again.
    // Update this snapshot only after a successful write, so failed saves
    // are retried even if their RAM value is already current.
    if (blob == g_persisted_blob) return kSaveDone;
    if (blob.size() > 4000)
    {
        ESP_LOGE(TAG, "kvflash payload too large (%u)", (unsigned)blob.size());
        return kSaveFail;
    }
    /* Every caller, including app-open on a blank board, only marks the
     * journal dirty. The erase runs later from FlushSettings, after the
     * screen transition has painted. An inline erase here latches the
     * panel blue on a board whose journal is still empty. */
    g_flush_needed = true;
    return kSaveDefer;
}

static void LoadKvFlashLocked()
{
    if (g_kvflash_loaded)
    {
        return;
    }
    if (OnUiThread())
        return;
    g_kvflash_loaded = true;

    char buf[4000];
    int n = metalio_kvflash_read(buf, sizeof(buf));
    if (n < 0)
    {
        if (n != -ENOENT)
        {
            ESP_LOGW(TAG, "kvflash read: %d", n);
        }
        return;
    }
    DeserializeAllLocked(buf, n);
    g_persisted_blob = SerializeAllLocked();
    ESP_LOGE(TAG, "kvflash: restored %d bytes", n);
    write(1, "KVFLASH_LD\n", 11);
}

/****************************************************************************/

/* Returns persist directory, or nullptr when using SPI flash kvflash.
 *
 * Never use /tmp or /var: they are RAM-only on this board and vanish on
 * power-off.  Never use /sdcard: NuttX vfat Slot0 FAT writes can wedge the
 * shared SDMMC host.  Always use metalio_kvflash_*.
 */
static const char *PersistDir(void)
{
    if (g_persist_dir_resolved)
    {
        return g_persist_dir[0] != '\0' ? g_persist_dir : nullptr;
    }

    g_persist_dir_resolved = true;
    g_persist_dir[0] = '\0';
    g_use_kvflash = true;
    return nullptr;
}

static void LoadFileLocked(NsStore &ns, const char *path)
{
    FILE *in = fopen(path, "r");
    if (in == nullptr)
    {
        ns.loaded = true;
        return;
    }

    char buf[512];
    while (fgets(buf, sizeof(buf), in) != nullptr)
    {
        std::string line(buf);
        if (!line.empty() && line.back() == '\n')
        {
            line.pop_back();
        }
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            continue;
        }
        ns.kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    fclose(in);
    ns.loaded = true;
}

static bool WriteFileLocked(const NsStore &ns, const char *path)
{
    FILE *out = fopen(path, "w");
    if (out == nullptr)
    {
        return false;
    }
    for (const auto &kv : ns.kv)
    {
        fputs(kv.first.c_str(), out);
        fputc('=', out);
        fputs(kv.second.c_str(), out);
        fputc('\n', out);
    }
    fclose(out);
    return true;
}

static NsStore &EnsureNsLocked(const std::string &name, const std::string &path)
{
    NsStore &ns = g_store[name];
    if (!ns.loaded)
    {
        if (!path.empty())
        {
            LoadFileLocked(ns, path.c_str());
        }
        else
        {
            ns.loaded = true;
        }
    }
    return ns;
}

/****************************************************************************/

Settings::Settings(const std::string &ns, bool read_write)
    : ns_(ns), read_write_(read_write), dirty_(false)
{
    pthread_mutex_lock(&g_lock);
    const char *dir = PersistDir();
    if (dir != nullptr)
    {
        path_ = std::string(dir) + "/" + ns_ + ".kv";
    }
    else if (g_use_kvflash)
    {
        LoadKvFlashLocked();
    }
    EnsureNsLocked(ns_, path_);
    pthread_mutex_unlock(&g_lock);
}

Settings::~Settings()
{
    if (dirty_ && read_write_)
    {
        Save();
    }
}

void Settings::Load()
{
}

std::string Settings::Find(const std::string &key) const
{
    pthread_mutex_lock(&g_lock);
    auto it = g_store.find(ns_);
    std::string v;
    if (it != g_store.end())
    {
        auto kit = it->second.kv.find(key);
        if (kit != it->second.kv.end())
        {
            v = kit->second;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return v;
}

void SettingsPrimeFlash()
{
    pthread_mutex_lock(&g_lock);
    PersistDir();
    LoadKvFlashLocked();
    pthread_mutex_unlock(&g_lock);
}

void Settings::Save()
{
    pthread_mutex_lock(&g_lock);
    if (g_use_kvflash)
    {
        const SaveAction action = SaveKvFlashLocked();
        pthread_mutex_unlock(&g_lock);
        if (action == kSaveDefer)
            KickFlush();
        if (action != kSaveFail)
        {
            dirty_ = false;
        }
        return;
    }
    if (path_.empty())
    {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    auto it = g_store.find(ns_);
    if (it == g_store.end())
    {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    bool ok = WriteFileLocked(it->second, path_.c_str());
    pthread_mutex_unlock(&g_lock);
    if (ok)
    {
        dirty_ = false;
    }
}

std::string Settings::GetString(const std::string &key,
                                const std::string &default_value)
{
    std::string v = Find(key);
    if (v.empty())
    {
        return default_value;
    }
    return v;
}

void Settings::SetString(const std::string &key, const std::string &value)
{
    if (!read_write_)
    {
        ESP_LOGW(TAG, "SetString('%s') on read-only namespace '%s'",
                 key.c_str(), ns_.c_str());
        return;
    }

    pthread_mutex_lock(&g_lock);
    NsStore &ns = EnsureNsLocked(ns_, path_);
    ns.kv[key] = value;

    SaveAction action = kSaveDone;
    bool persisted = false;
    if (g_use_kvflash)
    {
        action = SaveKvFlashLocked();
        persisted = action != kSaveFail;
    }
    else if (!path_.empty())
    {
        persisted = WriteFileLocked(ns, path_.c_str());
    }
    else if (!g_persist_warned)
    {
        g_persist_warned = true;
        pthread_mutex_unlock(&g_lock);
        ESP_LOGE(TAG, "no writable FS; RAM store this boot");
        dirty_ = false;
        return;
    }
    pthread_mutex_unlock(&g_lock);
    if (action == kSaveDefer)
        KickFlush();
    dirty_ = !persisted;
    if (persisted)
    {
        ESP_LOGI(TAG, "persisted %s.%s", ns_.c_str(), key.c_str());
    }
}

bool Settings::SetStringSaved(const std::string &key,const std::string &value)
{
    SetString(key,value);return read_write_&&!dirty_&&(g_use_kvflash||!path_.empty());
}

int32_t Settings::GetInt(const std::string &key, int32_t default_value)
{
    std::string v = Find(key);
    if (v.empty())
    {
        return default_value;
    }
    return static_cast<int32_t>(std::strtol(v.c_str(), nullptr, 10));
}

void Settings::SetInt(const std::string &key, int32_t value)
{
    SetString(key, std::to_string(value));
}

bool Settings::SetInts(const char *const *keys, const int32_t *values, size_t count)
{
    if (!read_write_ || !keys || !values) return false;
    pthread_mutex_lock(&g_lock);
    NsStore &ns = EnsureNsLocked(ns_, path_);
    for (size_t i = 0; i < count; ++i)
        ns.kv[keys[i]] = std::to_string(values[i]);
    SaveAction action = kSaveDone;
    bool saved = false;
    if (g_use_kvflash)
    {
        action = SaveKvFlashLocked();
        saved = action != kSaveFail;
    }
    else
        saved = !path_.empty() && WriteFileLocked(ns, path_.c_str());
    pthread_mutex_unlock(&g_lock);
    if (action == kSaveDefer)
        KickFlush();
    dirty_ = !saved;
    return saved;
}

bool Settings::GetBool(const std::string &key, bool default_value)
{
    std::string v = Find(key);
    if (v.empty())
    {
        return default_value;
    }
    return v == "1" || v == "true" || v == "yes";
}

void Settings::SetBool(const std::string &key, bool value)
{
    SetString(key, value ? "1" : "0");
}

void Settings::EraseKey(const std::string &key)
{
    if (!read_write_)
    {
        return;
    }

    pthread_mutex_lock(&g_lock);
    NsStore &ns = EnsureNsLocked(ns_, path_);
    ns.kv.erase(key);
    SaveAction action = kSaveDone;
    if (g_use_kvflash)
        action = SaveKvFlashLocked();
    else if (!path_.empty())
        WriteFileLocked(ns, path_.c_str());
    pthread_mutex_unlock(&g_lock);
    if (action == kSaveDefer)
        KickFlush();
}

void Settings::EraseAll()
{
    if (!read_write_)
    {
        return;
    }

    pthread_mutex_lock(&g_lock);
    NsStore &ns = EnsureNsLocked(ns_, path_);
    ns.kv.clear();
    SaveAction action = kSaveDone;
    if (g_use_kvflash)
    {
        action = SaveKvFlashLocked();
        pthread_mutex_unlock(&g_lock);
        if (action == kSaveDefer)
            KickFlush();
        return;
    }
    pthread_mutex_unlock(&g_lock);
    if (!path_.empty())
    {
        ::remove(path_.c_str());
    }
}
