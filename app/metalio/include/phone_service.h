#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace PhoneService {
struct Contact { std::string id,name,number; bool favorite=false; };
struct Call { std::string number,kind; int64_t when=0; int seconds=0; uint64_t id=0; };
struct Tone { std::string title,path; };
std::vector<Contact> Contacts();
std::string Initials(const std::string &name);
bool Matches(const Contact &contact,const std::string &query);
bool SaveContact(const Contact &contact);
void DeleteContact(const std::string &id);
std::vector<Call> History();
void ClearHistory();
bool DeleteHistory(uint64_t id);
void FlushHistory();
void DialStarted(const char *number);
void StateChanged(int state);
// Dedicated phone worker: modem RX never waits for AI, SD writes or audio setup.
void Init();
void PostStateChanged(int state);
void PostCallerNumber(const char *number);
void CallerNumber(const char *number);
std::string IncomingNumber();
std::string IncomingName();
std::string NumberRegion(const std::string &number);
std::vector<Tone> Ringtones();
std::string SelectedRingtone();
bool SelectRingtone(const std::string &path);
void StopRingtone();
void CancelRingtone();
}
