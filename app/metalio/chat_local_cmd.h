/*
 * Local chat voice commands — volume / backlight / bluetooth / open app.
 * Parsed from STT text on-device; does not use cloud MCP tools.
 */

#ifndef METALIO_CHAT_LOCAL_CMD_H
#define METALIO_CHAT_LOCAL_CMD_H

#include <string>

/* Returns true if `text` was a local device command and was executed. */
bool ChatLocalCmd_TryHandle(const std::string &text);

#endif /* METALIO_CHAT_LOCAL_CMD_H */
