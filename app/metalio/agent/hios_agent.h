// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
namespace HiOSAgent {
struct Reply { std::string session, text; bool ok = true; };
bool Initialize(std::string& error);
bool Send(const std::string& session, const std::string& text, std::string& error);
bool Poll(Reply& reply);
bool Busy();
std::string Config(const char *key);
bool SaveConfig(const char *key, const std::string& value);
bool SaveConfiguration(const std::vector<std::pair<std::string,std::string>>& values, std::string& error);
std::vector<std::string> Sessions();
std::string History(const std::string& session);
bool Clear(const std::string& session);
bool StartGateway(std::string& error);
void StopGateway();
bool StartRecording(const std::string& session, std::string& error);
void StopRecording(bool cancel = false);
bool PollTranscript(Reply& reply);
bool Speak(const std::string& text, std::string& error, const std::string& session = "");
void CancelVoice();
bool VoiceBusy();
}
