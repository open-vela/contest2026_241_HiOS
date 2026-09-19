/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Assets implementation — ported from MetalioClaw4 main/assets.cc.
 *
 * Substitutions from the ESP-IDF reference:
 *   - esp_partition_find_first / esp_partition_mmap → fopen/fread of a
 *     regular file at CONFIG_METALIO_ASSETS_FILE.
 *   - esp_partition_munmap                    → free(heap buffer).
 *   - esp_partition_erase_range + write       → fseek + fwrite.
 *   - spi_flash_mmap_get_free_pages           → not needed (heap-backed).
 *   - esp_partition_get_main_flash_sector_size → constant 4096.
 *   - Board::GetInstance().GetNetwork()->CreateHttp → local Http instance
 *     (see ota.h) over NuttX BSD sockets.
 *   - model_path.h (srmodel_load / esp_srmodel_deinit) → weak stubs.
 *
 * The bundle binary format is unchanged, so the same host-side packing
 * tool can produce bundles for both ESP-IDF and NuttX.
 */

#include "assets.h"
#include "board_shim.h"
#include "application.h"
#include "ota.h"
#include "esp_log_shim.h"
#include "esp_timer_shim.h"
#include "esp_err_shim.h"

#include "display.h"
#include "lvgl_theme.h"
#include "lvgl_font.h"
#include "lvgl_image.h"
#include "emoji_collection.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>

#ifdef CONFIG_METALIO_ESP_SR
#include "model_path.h"
#endif

#define TAG "Assets"

/* ------------------------------------------------------------------ */
/* On-disk bundle layout (must match the ESP-IDF reference)           */
/* ------------------------------------------------------------------ */

struct mmap_assets_table
{
    char asset_name[32];          /* Name of the asset */
    uint32_t asset_size;          /* Size of the asset */
    uint32_t asset_offset;        /* Offset of the asset */
    uint16_t asset_width;         /* Width of the asset */
    uint16_t asset_height;        /* Height of the asset */
};

/* Bundle header is 12 bytes: file_count, checksum, payload_len. */
#define ASSETS_HEADER_SIZE 12

/* Standard 4 KB sector — used only for progress reporting. */
#define ASSETS_SECTOR_SIZE 4096

/* ------------------------------------------------------------------ */
/* Construction / destruction                                         */
/* ------------------------------------------------------------------ */

Assets::Assets()
{
    assets_file_path_ = CONFIG_METALIO_ASSETS_FILE;
    default_assets_url_ = CONFIG_METALIO_ASSETS_URL;
    InitializePartition();
}

Assets::~Assets()
{
    if (mmap_root_ != nullptr)
    {
        free(mmap_root_);
        mmap_root_ = nullptr;
    }
}

uint32_t Assets::CalculateChecksum(const char *data, uint32_t length)
{
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; i++)
        checksum += (uint8_t)data[i];
    return checksum & 0xFFFF;
}

/* ------------------------------------------------------------------ */
/* InitializePartition — load + validate the on-disk bundle           */
/* ------------------------------------------------------------------ */

bool Assets::InitializePartition()
{
    partition_valid_ = false;
    checksum_valid_ = false;
    assets_.clear();

    if (mmap_root_ != nullptr)
    {
        free(mmap_root_);
        mmap_root_ = nullptr;
        mmap_size_ = 0;
    }

    FILE *fp = fopen(assets_file_path_.c_str(), "rb");
    if (fp == nullptr)
    {
        ESP_LOGI(TAG, "No assets file found at %s", assets_file_path_.c_str());
        return false;
    }

    /* Determine file size. */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0)
    {
        ESP_LOGE(TAG, "Assets file %s is empty or stat failed",
                 assets_file_path_.c_str());
        fclose(fp);
        return false;
    }

    ESP_LOGI(TAG, "Assets file %s: %ld bytes", assets_file_path_.c_str(), file_size);

    mmap_root_ = (char *)malloc((size_t)file_size);
    if (mmap_root_ == nullptr)
    {
        ESP_LOGE(TAG, "Failed to allocate %ld bytes for assets buffer", file_size);
        fclose(fp);
        return false;
    }
    mmap_size_ = (size_t)file_size;

    size_t nread = fread(mmap_root_, 1, mmap_size_, fp);
    fclose(fp);
    if (nread != mmap_size_)
    {
        ESP_LOGE(TAG, "Short read on assets file: %zu / %zu", nread, mmap_size_);
        free(mmap_root_);
        mmap_root_ = nullptr;
        mmap_size_ = 0;
        return false;
    }

    partition_valid_ = true;

    /* Parse the 12-byte header. */
    uint32_t stored_files = *(uint32_t *)(mmap_root_ + 0);
    uint32_t stored_chksum = *(uint32_t *)(mmap_root_ + 4);
    uint32_t stored_len = *(uint32_t *)(mmap_root_ + 8);

    if (stored_len > mmap_size_ - ASSETS_HEADER_SIZE)
    {
        ESP_LOGD(TAG, "stored_len (0x%lx) > file_size - 12 (0x%zx)",
                 (unsigned long)stored_len, mmap_size_ - ASSETS_HEADER_SIZE);
        return false;
    }

    auto start_time = esp_timer_get_time();
    uint32_t calculated_checksum = CalculateChecksum(mmap_root_ + ASSETS_HEADER_SIZE,
                                                     stored_len);
    auto end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "Checksum calculation time: %lld ms",
             (end_time - start_time) / 1000);

    if (calculated_checksum != stored_chksum)
    {
        ESP_LOGE(TAG, "Checksum mismatch: calculated 0x%lx != stored 0x%lx",
                 (unsigned long)calculated_checksum, (unsigned long)stored_chksum);
        return false;
    }

    checksum_valid_ = true;

    /* Build the name → Asset map. */
    for (uint32_t i = 0; i < stored_files; i++)
    {
        auto item = (const mmap_assets_table *)(mmap_root_ + ASSETS_HEADER_SIZE +
                                                i * sizeof(mmap_assets_table));
        Asset asset;
        asset.size = (size_t)item->asset_size;
        asset.offset = (size_t)(ASSETS_HEADER_SIZE +
                                sizeof(mmap_assets_table) * stored_files +
                                item->asset_offset);
        assets_[item->asset_name] = asset;
    }

    ESP_LOGI(TAG, "Assets loaded: %u files, payload %lu bytes",
             (unsigned)stored_files, (unsigned long)stored_len);
    return checksum_valid_;
}

/* ------------------------------------------------------------------ */
/* Apply — parse index.json and wire assets into the UI / audio       */
/* ------------------------------------------------------------------ */

bool Assets::Apply()
{
    void *ptr = nullptr;
    size_t size = 0;
    if (!GetAssetData("index.json", ptr, size))
    {
        ESP_LOGE(TAG, "index.json not found in asset bundle");
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(static_cast<char *>(ptr), size);
    if (root == nullptr)
    {
        ESP_LOGE(TAG, "index.json is not valid JSON");
        return false;
    }

    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(version))
    {
        if (version->valuedouble > 1)
        {
            ESP_LOGE(TAG, "Assets version %d not supported, upgrade firmware",
                     version->valueint);
            cJSON_Delete(root);
            return false;
        }
    }

    /* ---- Speech-recognition models ---- */
    cJSON *srmodels = cJSON_GetObjectItem(root, "srmodels");
    if (cJSON_IsString(srmodels))
    {
        std::string srmodels_file = srmodels->valuestring;
        if (GetAssetData(srmodels_file, ptr, size))
        {
            if (models_list_ != nullptr)
            {
                esp_srmodel_deinit(models_list_);
                models_list_ = nullptr;
            }
            models_list_ = srmodel_load(static_cast<uint8_t *>(ptr));
            if (models_list_ != nullptr)
            {
                auto &app = Application::GetInstance();
                app.GetAudioService().SetModelsList(models_list_);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to load srmodels.bin (stub?)");
            }
        }
        else
        {
            ESP_LOGE(TAG, "srmodels file %s not found", srmodels_file.c_str());
        }
    }

    /* ---- LVGL theme assets (fonts, emoji, skin) ----
     * The reference gates this on HAVE_LVGL.  The NuttX port always
     * builds with LVGL, so the branch is unconditional.  The
     * LcdDisplay-specific SetHideSubtitle block is omitted because the
     * port does not yet expose an LcdDisplay subclass. */
    auto &theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");

    cJSON *font = cJSON_GetObjectItem(root, "text_font");
    if (cJSON_IsString(font))
    {
        std::string fonts_text_file = font->valuestring;
        if (GetAssetData(fonts_text_file, ptr, size))
        {
            auto text_font = std::make_shared<LvglCBinFont>(ptr);
            if (text_font->font() == nullptr)
            {
                ESP_LOGE(TAG, "Failed to load fonts.bin");
                cJSON_Delete(root);
                return false;
            }
            if (light_theme != nullptr)
                light_theme->set_text_font(text_font);
            if (dark_theme != nullptr)
                dark_theme->set_text_font(text_font);
        }
        else
        {
            ESP_LOGE(TAG, "Font file %s not found", fonts_text_file.c_str());
        }
    }

    cJSON *emoji_collection = cJSON_GetObjectItem(root, "emoji_collection");
    if (cJSON_IsArray(emoji_collection))
    {
        auto custom_emoji_collection = std::make_shared<EmojiCollection>();
        int emoji_count = cJSON_GetArraySize(emoji_collection);
        for (int i = 0; i < emoji_count; i++)
        {
            cJSON *emoji = cJSON_GetArrayItem(emoji_collection, i);
            if (cJSON_IsObject(emoji))
            {
                cJSON *name = cJSON_GetObjectItem(emoji, "name");
                cJSON *file = cJSON_GetObjectItem(emoji, "file");
                cJSON *eaf = cJSON_GetObjectItem(emoji, "eaf");
                if (cJSON_IsString(name) && cJSON_IsString(file) &&
                    (eaf == nullptr))
                {
                    if (!GetAssetData(file->valuestring, ptr, size))
                    {
                        ESP_LOGE(TAG, "Emoji %s image file %s not found",
                                 name->valuestring, file->valuestring);
                        continue;
                    }
                    custom_emoji_collection->AddEmoji(name->valuestring,
                                                      std::make_shared<LvglRawImage>(ptr, size));
                }
            }
        }
        if (light_theme != nullptr)
            light_theme->set_emoji_collection(custom_emoji_collection);
        if (dark_theme != nullptr)
            dark_theme->set_emoji_collection(custom_emoji_collection);
    }

    cJSON *skin = cJSON_GetObjectItem(root, "skin");
    if (cJSON_IsObject(skin))
    {
        cJSON *light_skin = cJSON_GetObjectItem(skin, "light");
        if (cJSON_IsObject(light_skin) && light_theme != nullptr)
        {
            cJSON *text_color = cJSON_GetObjectItem(light_skin, "text_color");
            cJSON *background_color = cJSON_GetObjectItem(light_skin, "background_color");
            cJSON *background_image = cJSON_GetObjectItem(light_skin, "background_image");
            if (cJSON_IsString(text_color))
                light_theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
            if (cJSON_IsString(background_color))
            {
                light_theme->set_background_color(
                    LvglTheme::ParseColor(background_color->valuestring));
                light_theme->set_chat_background_color(
                    LvglTheme::ParseColor(background_color->valuestring));
            }
            if (cJSON_IsString(background_image))
            {
                if (!GetAssetData(background_image->valuestring, ptr, size))
                {
                    ESP_LOGE(TAG, "Background image %s not found",
                             background_image->valuestring);
                    cJSON_Delete(root);
                    return false;
                }
                auto bg_image = std::make_shared<LvglCBinImage>(ptr);
                light_theme->set_background_image(bg_image);
            }
        }
        cJSON *dark_skin = cJSON_GetObjectItem(skin, "dark");
        if (cJSON_IsObject(dark_skin) && dark_theme != nullptr)
        {
            cJSON *text_color = cJSON_GetObjectItem(dark_skin, "text_color");
            cJSON *background_color = cJSON_GetObjectItem(dark_skin, "background_color");
            cJSON *background_image = cJSON_GetObjectItem(dark_skin, "background_image");
            if (cJSON_IsString(text_color))
                dark_theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
            if (cJSON_IsString(background_color))
            {
                dark_theme->set_background_color(
                    LvglTheme::ParseColor(background_color->valuestring));
                dark_theme->set_chat_background_color(
                    LvglTheme::ParseColor(background_color->valuestring));
            }
            if (cJSON_IsString(background_image))
            {
                if (!GetAssetData(background_image->valuestring, ptr, size))
                {
                    ESP_LOGE(TAG, "Background image %s not found",
                             background_image->valuestring);
                    cJSON_Delete(root);
                    return false;
                }
                auto bg_image = std::make_shared<LvglCBinImage>(ptr);
                dark_theme->set_background_image(bg_image);
            }
        }
    }

    /* Refresh the active display theme so the new skin takes effect. */
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        ESP_LOGI(TAG, "Refreshing display theme...");
        auto current_theme = display->GetTheme();
        if (current_theme != nullptr)
            display->SetTheme(current_theme);
    }

    cJSON_Delete(root);
    return true;
}

/* ------------------------------------------------------------------ */
/* Download — fetch a new bundle over HTTP and replace the file       */
/* ------------------------------------------------------------------ */

bool Assets::Download(std::string url,
                      std::function<void(int progress, size_t speed)> progress_callback)
{
    ESP_LOGI(TAG, "Downloading new asset bundle from %s", url.c_str());

    /* Drop the in-memory copy so GetAssetData can't serve stale data
     * while the download is in flight. */
    if (mmap_root_ != nullptr)
    {
        free(mmap_root_);
        mmap_root_ = nullptr;
        mmap_size_ = 0;
    }
    checksum_valid_ = false;
    assets_.clear();

    /* Use the BSD-socket Http client from ota.h.  The reference uses
     * Board::GetInstance().GetNetwork()->CreateHttp(0); on NuttX the
     * Http class is standalone and works the moment the network stack
     * is up. */
    Http http;
    if (!http.Open("GET", url))
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http.GetStatusCode() != 200)
    {
        ESP_LOGE(TAG, "HTTP status %d fetching assets", http.GetStatusCode());
        http.Close();
        return false;
    }

    size_t content_length = http.GetBodyLength();
    if (content_length == 0)
    {
        ESP_LOGE(TAG, "Content-Length is 0");
        http.Close();
        return false;
    }

    /* Open the destination file for writing.  NuttX file I/O does not
     * require explicit erase — fwrite overwrites directly. */
    FILE *fp = fopen(assets_file_path_.c_str(), "wb");
    if (fp == nullptr)
    {
        ESP_LOGE(TAG, "Failed to open %s for writing", assets_file_path_.c_str());
        http.Close();
        return false;
    }

    ESP_LOGI(TAG, "Downloading %zu bytes to %s (sector %d)",
             content_length, assets_file_path_.c_str(), ASSETS_SECTOR_SIZE);

    char buffer[512];
    size_t total_written = 0;
    size_t recent_written = 0;
    size_t current_sector = 0;
    auto last_calc_time = esp_timer_get_time();

    while (true)
    {
        int ret = http.Read(buffer, sizeof(buffer));
        if (ret < 0)
        {
            ESP_LOGE(TAG, "HTTP read failed: %d", ret);
            fclose(fp);
            http.Close();
            return false;
        }
        if (ret == 0)
            break;

        /* Track sector boundaries for progress logging parity with the
         * ESP-IDF version (which had to erase sectors ahead of writes). */
        size_t write_end_offset = total_written + (size_t)ret;
        size_t needed_sectors =
            (write_end_offset + ASSETS_SECTOR_SIZE - 1) / ASSETS_SECTOR_SIZE;
        while (current_sector < needed_sectors)
            current_sector++;

        size_t written = fwrite(buffer, 1, (size_t)ret, fp);
        if (written != (size_t)ret)
        {
            ESP_LOGE(TAG, "fwrite short write at offset %zu", total_written);
            fclose(fp);
            http.Close();
            return false;
        }

        total_written += (size_t)ret;
        recent_written += (size_t)ret;

        if (esp_timer_get_time() - last_calc_time >= 1000000 ||
            total_written == content_length)
        {
            size_t progress = total_written * 100 / content_length;
            size_t speed = recent_written;  /* bytes per second */
            ESP_LOGI(TAG, "Progress: %zu%% (%zu/%zu), %zu B/s, sectors %zu",
                     progress, total_written, content_length, speed,
                     current_sector);
            if (progress_callback)
                progress_callback((int)progress, speed);
            last_calc_time = esp_timer_get_time();
            recent_written = 0;
        }
    }

    fclose(fp);
    http.Close();

    if (total_written != content_length)
    {
        ESP_LOGE(TAG, "Downloaded %zu != expected %zu", total_written, content_length);
        return false;
    }

    ESP_LOGI(TAG, "Download complete: %zu bytes, %zu sectors",
             total_written, current_sector);

    /* Re-load the new bundle. */
    if (!InitializePartition())
    {
        ESP_LOGE(TAG, "Failed to re-initialize assets from %s",
                 assets_file_path_.c_str());
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* GetAssetData — look up an asset by name and return its payload     */
/* ------------------------------------------------------------------ */

bool Assets::GetAssetData(const std::string &name, void *&ptr, size_t &size)
{
    auto asset = assets_.find(name);
    if (asset == assets_.end())
        return false;

    if (mmap_root_ == nullptr)
        return false;

    auto data = (const char *)(mmap_root_ + asset->second.offset);
    if (data[0] != 'Z' || data[1] != 'Z')
    {
        ESP_LOGE(TAG, "Asset %s has bad magic %02x%02x",
                 name.c_str(), (uint8_t)data[0], (uint8_t)data[1]);
        return false;
    }

    ptr = static_cast<void *>(const_cast<char *>(data + 2));
    size = asset->second.size;
    return true;
}
