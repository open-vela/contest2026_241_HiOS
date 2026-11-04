// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lvgl.h"
#include <string>
namespace HiApp {
// UI thread only. Downloads and decoding run on one bounded low-priority worker.
lv_obj_t *WebImageCreate(lv_obj_t *parent,const std::string &url,
                        const std::string &page,const std::string &alt,bool network);
}
