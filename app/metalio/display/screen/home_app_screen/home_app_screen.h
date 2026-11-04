/*
 * Home — a small Mi Home style app for the devices on this panel.
 */

#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class HomeAppScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "home"; }

    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
