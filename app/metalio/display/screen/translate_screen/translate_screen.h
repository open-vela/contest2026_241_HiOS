// SPDX-License-Identifier: Apache-2.0
// Translation app UI only; no recording or service client.
#pragma once

#include "screen_base.h"
#include "screen_util.h"
#include "lvgl.h"

class TranslateScreen : public Screen
{
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "translate"; }

    // Legacy static entry point — used by home_screen / metalio_main
    // callers that still use the original MetalioClaw4 signature.
    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
