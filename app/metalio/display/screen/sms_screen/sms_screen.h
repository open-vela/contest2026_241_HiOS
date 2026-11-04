#pragma once
#include "screen_base.h"
#include "screen_util.h"
#include <string>
class SmsScreen:public Screen {
public:
 lv_obj_t *Create() override {return root_=CreateStatic();}
 const char *Name()const override{return "sms";}
 static void InitGlobal();
 static bool IsForeground();
 static void OpenConversation(const std::string &number);
 static lv_obj_t *CreateStatic();
 static lv_obj_t *CreateIcon(lv_obj_t *parent,int size);
};
