#ifndef METALIO_SETTINGS_H
#define METALIO_SETTINGS_H

#include <string>
#include <cstdint>

/**
 * NuttX-compatible Settings class.
 *
 * Drop-in replacement for the ESP-IDF NVS-based Settings.  Process-wide RAM
 * is the source of truth so OTA mqtt/websocket values survive until
 * MqttProtocol::Start() on the same boot.  Optionally persisted under
 * /tmp or /var (never /sdcard — Slot0 FAT writes can wedge shared SDMMC).
 * File format: key=value (one pair per line; key may not contain '=').
 *
 * Usage mirrors the original:
 *   Settings s("ui", true);   // read-write
 *   s.SetString("locale", "en-US");
 *   auto v = s.GetString("locale", "zh-CN");
 */
class Settings
{
public:
    Settings(const std::string &ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string &key,
                          const std::string &default_value = "");
    void SetString(const std::string &key, const std::string &value);

    int32_t GetInt(const std::string &key, int32_t default_value = 0);
    void SetInt(const std::string &key, int32_t value);

    bool GetBool(const std::string &key, bool default_value = false);
    void SetBool(const std::string &key, bool value);

    void EraseKey(const std::string &key);
    void EraseAll();

private:
    std::string ns_;
    bool read_write_;
    bool dirty_;
    std::string path_;

    void Load();
    void Save();
    std::string Find(const std::string &key) const;
};

#endif // METALIO_SETTINGS_H
