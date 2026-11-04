#pragma once
#include <string>
#include <functional>
namespace NotificationSound {
enum class Kind{Sms,Alarm};
std::string Selected(Kind);
bool Select(Kind,const std::string&);
void SetSystemMode(int mode);
void Sms();
void Alarm(const std::string& path="",std::function<bool()> allowed={});
void StopAlarm();
}
