#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include "cJSON_compat.h"
namespace DesktopFolders {
struct Folder{std::string id,name;std::vector<std::string> items;};
struct Model{bool loaded=false;std::vector<std::string> desktop;std::vector<Folder> folders;};
inline bool IsFolder(const std::string& id){return id.compare(0,7,"folder:")==0;}
inline bool Id(const std::string& id){return !id.empty()&&id.size()<=40&&id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-:")==id.npos;}
inline bool Name(const std::string& name){if(name.empty()||name.size()>64||name.find_first_of("\r\n\t")!=name.npos)return false;for(unsigned char c:name)if(c<32)return false;return name.find_first_not_of(' ')!=name.npos;}
inline const Folder* Find(const Model& model,const std::string& id){for(const auto& f:model.folders)if(f.id==id)return &f;return nullptr;}
inline bool Grouped(const Model& model,const std::string& id){for(const auto& f:model.folders)if(std::find(f.items.begin(),f.items.end(),id)!=f.items.end())return true;return false;}
inline bool Valid(const Model& model){
    if(model.desktop.size()>256||model.folders.size()>8)return false;std::vector<std::string> seen,folder_ids;
    for(const auto& f:model.folders){if(!Id(f.id)||!IsFolder(f.id)||!Name(f.name)||f.items.size()<2||f.items.size()>64||std::find(folder_ids.begin(),folder_ids.end(),f.id)!=folder_ids.end())return false;
        folder_ids.push_back(f.id);for(const auto& id:f.items){if(!Id(id)||IsFolder(id)||std::find(seen.begin(),seen.end(),id)!=seen.end())return false;seen.push_back(id);}}
    std::vector<std::string> desktop_seen;
    for(const auto& id:model.desktop){if(!Id(id)||std::find(desktop_seen.begin(),desktop_seen.end(),id)!=desktop_seen.end())return false;desktop_seen.push_back(id);
        if(IsFolder(id)){if(!Find(model,id))return false;}else{if(std::find(seen.begin(),seen.end(),id)!=seen.end())return false;seen.push_back(id);}}
    for(const auto& id:folder_ids)if(std::find(model.desktop.begin(),model.desktop.end(),id)==model.desktop.end())return false;return true;
}
inline bool Drop(const Model& original,const std::string& dragged,const std::string& target,const std::string& new_id,Model& result,std::string& folder_id){
    if(dragged==target||IsFolder(dragged)||!Id(dragged))return false;auto next=original;next.loaded=true;
    auto from=std::find(next.desktop.begin(),next.desktop.end(),dragged),to=std::find(next.desktop.begin(),next.desktop.end(),target);
    if(from==next.desktop.end()||to==next.desktop.end())return false;
    if(IsFolder(target)){bool found=false;for(auto& f:next.folders)if(f.id==target){if(f.items.size()>=64)return false;f.items.push_back(dragged);found=true;break;}if(!found)return false;folder_id=target;}
    else {if(next.folders.size()>=8||!IsFolder(new_id)||Find(next,new_id))return false;Folder folder;folder.id=new_id;folder.name="文件夹";folder.items.push_back(target);folder.items.push_back(dragged);next.folders.push_back(std::move(folder));folder_id=new_id;}
    next.desktop.erase(std::find(next.desktop.begin(),next.desktop.end(),dragged));
    if(!IsFolder(target))*std::find(next.desktop.begin(),next.desktop.end(),target)=folder_id;
    if(!Valid(next))return false;result=std::move(next);return true;
}
inline bool Rename(const Model& original,const std::string& id,const std::string& name,Model& result){
    if(!Name(name))return false;auto next=original;bool found=false;for(auto& f:next.folders)if(f.id==id){f.name=name;found=true;}
    if(!found||!Valid(next))return false;result=std::move(next);return true;
}
inline bool Reorder(const Model& original,const std::string& id,const std::vector<std::string>& order,Model& result){
    auto next=original;bool found=false;
    for(auto& f:next.folders)if(f.id==id){
        auto before=f.items,after=order;std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
        if(before!=after)return false;f.items=order;found=true;
    }
    if(!found||!Valid(next))return false;result=std::move(next);return true;
}
inline bool Remove(const Model& original,const std::string& id,const std::string& app,Model& result){
    auto next=original;auto position=std::find(next.desktop.begin(),next.desktop.end(),id);if(position==next.desktop.end())return false;const size_t slot=position-next.desktop.begin();
    for(size_t i=0;i<next.folders.size();++i){auto& f=next.folders[i];if(f.id!=id)continue;auto item=std::find(f.items.begin(),f.items.end(),app);if(item==f.items.end())return false;f.items.erase(item);
        if(f.items.size()==1){next.desktop[slot]=f.items[0];next.folders.erase(next.folders.begin()+i);}next.desktop.insert(next.desktop.begin()+slot+1,app);
        if(!Valid(next))return false;result=std::move(next);return true;}return false;
}
inline bool Ungroup(const Model& original,const std::string& id,Model& result){
    auto next=original;auto position=std::find(next.desktop.begin(),next.desktop.end(),id);if(position==next.desktop.end())return false;size_t slot=position-next.desktop.begin();
    for(size_t i=0;i<next.folders.size();++i)if(next.folders[i].id==id){auto items=next.folders[i].items;next.desktop.erase(next.desktop.begin()+slot);next.desktop.insert(next.desktop.begin()+slot,items.begin(),items.end());next.folders.erase(next.folders.begin()+i);
        if(!Valid(next))return false;result=std::move(next);return true;}return false;
}
inline std::string Encode(const Model& model){
    if(!Valid(model))return {};cJSON* root=cJSON_CreateObject();if(!root)return {};cJSON_AddNumberToObject(root,"version",1);auto* desktop=cJSON_AddArrayToObject(root,"desktop");
    for(const auto& id:model.desktop)cJSON_AddItemToArray(desktop,cJSON_CreateString(id.c_str()));auto* folders=cJSON_AddArrayToObject(root,"folders");
    for(const auto& f:model.folders){auto* item=cJSON_CreateObject();cJSON_AddStringToObject(item,"id",f.id.c_str());cJSON_AddStringToObject(item,"name",f.name.c_str());auto* members=cJSON_AddArrayToObject(item,"items");for(const auto& id:f.items)cJSON_AddItemToArray(members,cJSON_CreateString(id.c_str()));cJSON_AddItemToArray(folders,item);}
    char* encoded=cJSON_PrintUnformatted(root);cJSON_Delete(root);std::string raw=encoded?encoded:"";cJSON_free(encoded);return raw.size()<=2600?raw:std::string();
}
inline bool Decode(const std::string& raw,Model& result){
    if(raw.empty()||raw.size()>2600)return false;cJSON* root=cJSON_Parse(raw.c_str());if(!root)return false;auto* version=cJSON_GetObjectItemCaseSensitive(root,"version"),*desktop=cJSON_GetObjectItemCaseSensitive(root,"desktop"),*folders=cJSON_GetObjectItemCaseSensitive(root,"folders");
    Model next;next.loaded=true;bool ok=cJSON_IsNumber(version)&&version->valueint==1&&cJSON_IsArray(desktop)&&cJSON_IsArray(folders)&&cJSON_GetArraySize(desktop)<=256&&cJSON_GetArraySize(folders)<=8;
    if(ok){for(int i=0;i<cJSON_GetArraySize(desktop);++i){auto* item=cJSON_GetArrayItem(desktop,i);if(!cJSON_IsString(item)){ok=false;break;}next.desktop.push_back(item->valuestring);}}
    if(ok)for(int i=0;i<cJSON_GetArraySize(folders);++i){auto* item=cJSON_GetArrayItem(folders,i);auto* id=cJSON_GetObjectItemCaseSensitive(item,"id"),*name=cJSON_GetObjectItemCaseSensitive(item,"name"),*members=cJSON_GetObjectItemCaseSensitive(item,"items");
        if(!cJSON_IsString(id)||!cJSON_IsString(name)||!cJSON_IsArray(members)||cJSON_GetArraySize(members)>64){ok=false;break;}Folder f;f.id=id->valuestring;f.name=name->valuestring;
        for(int j=0;j<cJSON_GetArraySize(members);++j){auto* member=cJSON_GetArrayItem(members,j);if(!cJSON_IsString(member)){ok=false;break;}f.items.push_back(member->valuestring);}next.folders.push_back(std::move(f));}
    cJSON_Delete(root);if(!ok||!Valid(next))return false;result=std::move(next);return true;
}
Model Snapshot();void Commit(const Model& model);bool Save(const Model& model);std::string NewId();
}
