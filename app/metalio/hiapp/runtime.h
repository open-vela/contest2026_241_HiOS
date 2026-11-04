// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lvgl.h"
#include <string>
namespace HiApp {
lv_obj_t* CreateRuntime(const std::string&id);
bool Launch(const char*id); // queues work on the LVGL thread
bool IsLaunchId(const char*id);
}
