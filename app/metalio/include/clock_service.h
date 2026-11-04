#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace ClockService {
struct Alarm{int id=0,hour=7,minute=30,days=0;bool enabled=true;std::string label="闹钟",tone;};
struct Snapshot{std::vector<Alarm> alarms;std::vector<int> cities;std::vector<int64_t> laps;int64_t stopwatch=0,timer=0,timer_total=60000;bool stopwatch_running=false,timer_running=false;int alert=0;std::string alert_label,status;unsigned revision=0;};
struct AlertSnapshot{int alert=0;std::string alert_label;};
AlertSnapshot GetAlert();
void Init();Snapshot Get();bool SaveAlarm(Alarm);void RemoveAlarm(int);void ToggleAlarm(int);void Cities(const std::vector<int>&);
void Stopwatch(int command); // 0 toggle, 1 reset, 2 lap
void Timer(int command,int64_t duration=0); // 0 toggle, 1 reset, 2 set
void Dismiss(bool snooze=false);
// Both the Clock picker and Settings select one persistent system alarm tone.
void UseSystemAlarmTone();
}
