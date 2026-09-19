/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Assets — ported from MetalioClaw4 main/assets.h.
 *
 * The reference implementation memory-maps a dedicated ESP-IDF flash
 * partition that holds a binary asset bundle (fonts, emoji images,
 * srmodels, theme skin).  On NuttX we keep the same on-disk bundle
 * format but read it from a regular file on the root filesystem:
 *
 *   CONFIG_METALIO_ASSETS_FILE (default "/etc/metalio/assets.bin")
 *
 * The bundle layout is preserved verbatim so the same host-side packing
 * tool can produce bundles for both ESP-IDF and NuttX:
 *
 *   offset 0   : uint32_t stored_files   (number of asset table entries)
 *   offset 4   : uint32_t stored_chksum  (sum of all payload bytes, &0xFFFF)
 *   offset 8   : uint32_t stored_len     (total bytes of payload data)
 *   offset 12  : asset table  (stored_files * sizeof(mmap_assets_table))
 *   after table: asset payload data (each asset prefixed with 'ZZ' magic)
 *
 * Download() fetches a new bundle over HTTP and atomically replaces the
 * file; Apply() parses index.json from the bundle and wires fonts /
 * emoji / skin into the LvglThemeManager + AudioService.
 */

#ifndef ASSETS_H
#define ASSETS_H

#include <map>
#include <string>
#include <functional>
#include <cstddef>

#include "cJSON_compat.h"

#if defined(CONFIG_METALIO_ESP_SR) || defined(METALIO_HAS_ESP_SR)
#include "model_path.h"
#else
/* ESP-SR model list type — forward-declared until ESP-SR is linked. */
struct srmodel_list_t;
extern "C" srmodel_list_t *srmodel_load(uint8_t *data);
extern "C" void esp_srmodel_deinit(srmodel_list_t *models);
#endif

/* Default path of the asset bundle on the NuttX root filesystem. */
#ifndef CONFIG_METALIO_ASSETS_FILE
#define CONFIG_METALIO_ASSETS_FILE "/etc/metalio/assets.bin"
#endif

/* Default URL for downloading the asset bundle.  Empty by default —
 * callers must pass a URL to Download(). */
#ifndef CONFIG_METALIO_ASSETS_URL
#define CONFIG_METALIO_ASSETS_URL ""
#endif

struct Asset
{
    size_t size;
    size_t offset;
};

class Assets
{
public:
    static Assets &GetInstance()
    {
        static Assets instance;
        return instance;
    }
    ~Assets();

    bool Download(std::string url,
                  std::function<void(int progress, size_t speed)> progress_callback);
    bool Apply();
    bool GetAssetData(const std::string &name, void *&ptr, size_t &size);

    inline bool partition_valid() const { return partition_valid_; }
    inline bool checksum_valid() const { return checksum_valid_; }
    inline std::string default_assets_url() const { return default_assets_url_; }

private:
    Assets();
    Assets(const Assets &) = delete;
    Assets &operator=(const Assets &) = delete;

    bool InitializePartition();
    uint32_t CalculateChecksum(const char *data, uint32_t length);

    std::string assets_file_path_;
    char *mmap_root_ = nullptr;   /* heap buffer holding the bundle */
    size_t mmap_size_ = 0;
    bool partition_valid_ = false;
    bool checksum_valid_ = false;
    std::string default_assets_url_;
    srmodel_list_t *models_list_ = nullptr;
    std::map<std::string, Asset> assets_;
};

#endif /* ASSETS_H */
