/*
 * SPDX-FileCopyrightText: 2024 openvela port
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emoji collection — ported from MetalioClaw4 main/display/lvgl_display/emoji_collection.h.
 * Maps emotion names to LVGL image sources.
 */

#pragma once

#include "lvgl_image.h"
#include <lvgl.h>
#include <string>
#include <map>
#include <memory>

class EmojiCollection
{
public:
    EmojiCollection() = default;
    ~EmojiCollection() = default;

    /* Add an emoji image for a given emotion name */
    void AddEmoji(const std::string &name, std::shared_ptr<LvglImage> image)
    {
        emojis_[name] = image;
    }

    /* Get the image for an emotion, or nullptr if not found */
    std::shared_ptr<LvglImage> GetEmoji(const std::string &name) const
    {
        auto it = emojis_.find(name);
        if (it != emojis_.end())
        {
            return it->second;
        }
        return nullptr;
    }

    /* Check if an emotion exists */
    bool HasEmoji(const std::string &name) const
    {
        return emojis_.find(name) != emojis_.end();
    }

private:
    std::map<std::string, std::shared_ptr<LvglImage>> emojis_;
};
