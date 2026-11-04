/*
 * Desktop shown at boot: date, weather, pinned device cards, and a dock.
 * The 9-grid launcher lives behind the folder icon.
 */

#pragma once

#include "screen_base.h"
#include "lvgl.h"

class PanelScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "panel"; }

    static lv_obj_t *CreateStatic();
    /* Load the smart-home panel. */
    static lv_obj_t *Show();
    /* Switch from the smart-home panel to the app grid without keeping both
     * large LVGL screen trees allocated at the same time. */
    static lv_obj_t *OpenLauncher();
    static bool IsForeground();
    /* Repaint pinned cards after a voice command. Safe off the UI thread. */
    static void RefreshDevices();
    static bool IsPanelObject(lv_obj_t *obj);
};
