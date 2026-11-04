#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace SmsService {
// Convert canonical stored UTC to the existing system display clock.
int64_t DisplayTime(int64_t utc);
enum class State { Received,Queued,Sent,Failed,Uncertain,Cancelled };
struct Message {
    std::string id,number,text,error;
    int64_t when=0;
    int64_t cancel_until_ms=0; // Monotonic, never restored as a send permission.
    State state=State::Received;
    bool unread=false,deleted=false;
};
struct ThreadOptions {std::string number;bool pinned=false,blocked=false;};
std::vector<ThreadOptions> ConversationOptions();
void SetPinned(const std::string &number,bool pinned);
void SetBlocked(const std::string &number,bool blocked);
void DeleteConversation(const std::string &number);
bool CancelSend(const std::string &id);
int64_t MonotonicMillis();
void Init();
void Refresh();
unsigned Revision();
std::string Status();
std::vector<Message> Messages();
bool Send(const std::string &number,const std::string &text);
// Only newly received, durably saved messages; boot inbox sync is deduplicated.
bool TakeNotification(Message &message,unsigned &count);
void MarkRead(const std::string &number);
void Delete(const std::string &id);
std::string Draft();
void SaveDraft(const std::string &number,const std::string &text);
std::string NormalizeNumber(const std::string &number);
}
