// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "screen_base.h"
#include "screen_util.h"
class AppManagerScreen:public Screen {
public:
 lv_obj_t*Create()override{return root_=CreateStatic();}
 const char*Name()const override{return "app_manager";}
 static lv_obj_t*CreateStatic();
 static lv_obj_t*CreateIcon(lv_obj_t*,int);
};
