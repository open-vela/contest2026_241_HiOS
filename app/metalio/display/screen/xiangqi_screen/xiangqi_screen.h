// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "screen_base.h"
#include "screen_util.h"

class XiangqiScreen : public Screen {
public:
    lv_obj_t *Create() override;
    const char *Name() const override { return "xiangqi"; }
    static lv_obj_t *CreateStatic();
    static void LifecycleCallback(screen_lifecycle_event_t event);
    static lv_obj_t *CreateIcon(lv_obj_t *parent, int size);
};
