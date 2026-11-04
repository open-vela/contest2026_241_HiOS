// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "hiapp.h"
#include "lvgl.h"
namespace HiApp {
void RegistryStart();
void RegistryPublish(const std::vector<Record>&,unsigned expected_revision=0);
void RegistryUpsert(const Record&);
void RegistryRemove(const std::string&);
unsigned RegistryRevision();
std::vector<Record> RegistrySnapshot();
void RegistryLoadOrder();
std::vector<std::string> RegistryOrder(bool*loaded=nullptr);
void RegistrySaveOrder(const std::vector<std::string>&);
lv_obj_t* RecordIcon(lv_obj_t*,const Record&,int,bool in_slot=false);
lv_obj_t* RegistryIcon(lv_obj_t*parent,const char*id,int size,bool in_slot=false);
}
