#pragma once
#include "screen_base.h"
#include "screen_util.h"
class CompassScreen:public Screen{public:lv_obj_t*Create()override{return root_=CreateStatic();}const char*Name()const override{return "compass";}static lv_obj_t*CreateStatic();static lv_obj_t*CreateIcon(lv_obj_t*,int);};
