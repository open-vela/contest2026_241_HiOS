#include "desktop_folders.h"
#include "settings.h"
#include "esp_timer_shim.h"
#include <mutex>
#include <cstdio>
namespace DesktopFolders {namespace {
struct Store{std::mutex lock;Model model;bool initialized=false;};
Store& Data(){static auto* store=new Store;return *store;}
}
Model Snapshot(){auto& s=Data();std::lock_guard<std::mutex> guard(s.lock);if(!s.initialized){Settings cfg("ui");Decode(cfg.GetString("desktop_layout",""),s.model);s.initialized=true;}return s.model;}
void Commit(const Model& model){auto& s=Data();std::lock_guard<std::mutex> guard(s.lock);s.model=model;s.initialized=true;}
bool Save(const Model& model){auto raw=Encode(model);if(raw.empty())return false;Settings cfg("ui",true);const auto before=cfg.GetString("desktop_layout","");if(cfg.SetStringSaved("desktop_layout",raw))return true;cfg.SetStringSaved("desktop_layout",before);return false;}
std::string NewId(){static unsigned counter=0;const auto model=Snapshot();for(;;){auto tick=uint64_t(esp_timer_get_time());char id[40];snprintf(id,sizeof(id),"folder:%08lx%08lx",(unsigned long)(tick>>32),(unsigned long)(uint32_t(tick)^++counter));if(!Find(model,id))return id;}}
}
