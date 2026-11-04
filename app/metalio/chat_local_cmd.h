/*
 * Local chat voice commands — volume / backlight / bluetooth / open app / call.
 * Parsed from STT text on-device; does not use cloud MCP tools.
 */

#ifndef METALIO_CHAT_LOCAL_CMD_H
#define METALIO_CHAT_LOCAL_CMD_H

#include <string>

/* Returns true if `text` was a local device command and was executed. */
bool ChatLocalCmd_TryHandle(const std::string &text);

/* Dial `name_or_number` from the contact book as it exists right now.
 * Later additions are visible on the next call. Returns false when the
 * name is empty. A missing or ambiguous name is still handled locally. */
bool ChatLocalCmd_CallContact(const std::string &name_or_number);

/* Hi 视界 spoken-name aliases, shared by local STT and MCP app opening.
 * Only explicit open commands are normalized for the user transcript. */
bool ChatLocalCmd_IsVisionAppName(const std::string &name);
std::string ChatLocalCmd_NormalizeVisionOpenCommand(const std::string &text);
/* One-shot permission from the latest explicit user utterance, never from LLM text. */
bool ChatLocalCmd_ConsumeVisionOpenRequest();

#endif /* METALIO_CHAT_LOCAL_CMD_H */
