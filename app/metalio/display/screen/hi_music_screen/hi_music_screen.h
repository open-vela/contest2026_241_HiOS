#pragma once

#include "lvgl.h"
#include <string>
#include "screen_base.h"
#include "screen_util.h"
#include "pwr_key_handler/pwr_key_handler.h"

class HiMusicScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "hi_music"; }
    static lv_obj_t *CreateStatic();
    static bool OpenFile(const char *path);
    static bool PlayLocalQuery(const std::string &query);
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
