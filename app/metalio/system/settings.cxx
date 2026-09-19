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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <vector>

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
static char g_persist_dir[64];

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

static bool SaveKvFlashLocked()
{
    std::string blob = SerializeAllLocked();
    if (blob.size() > 4000)
    {
        ESP_LOGE(TAG, "kvflash payload too large (%u)", (unsigned)blob.size());
        return false;
    }
    int rc = metalio_kvflash_write(blob.data(), blob.size());
    if (rc < 0)
    {
        ESP_LOGE(TAG, "kvflash write failed: %d", rc);
        return false;
    }
    write(1, "KVFLASH_OK\n", 11);
    return true;
}

static void LoadKvFlashLocked()
{
    if (g_kvflash_loaded)
    {
        return;
    }
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

void Settings::Save()
{
    pthread_mutex_lock(&g_lock);
    if (g_use_kvflash)
    {
        bool ok = SaveKvFlashLocked();
        pthread_mutex_unlock(&g_lock);
        if (ok)
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

    bool persisted = false;
    if (g_use_kvflash)
    {
        persisted = SaveKvFlashLocked();
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
    dirty_ = !persisted;
    if (persisted)
    {
        ESP_LOGI(TAG, "persisted %s.%s", ns_.c_str(), key.c_str());
    }
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
    if (g_use_kvflash)
    {
        SaveKvFlashLocked();
    }
    else if (!path_.empty())
    {
        WriteFileLocked(ns, path_.c_str());
    }
    pthread_mutex_unlock(&g_lock);
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
    if (g_use_kvflash)
    {
        SaveKvFlashLocked();
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_mutex_unlock(&g_lock);
    if (!path_.empty())
    {
        ::remove(path_.c_str());
    }
}
