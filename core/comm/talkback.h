// SPDX-License-Identifier: MIT
#ifndef GM_TALKBACK_H
#define GM_TALKBACK_H
#include "app/platform.h"
namespace gm {
struct RemoteCommand {
  uint32_t minutes;
  uint32_t validUntil;
};
// Strict decimal grammar, whitespace allowed around tokens; 0 = no expiry.
bool talkbackParseCommand(const char* text, RemoteCommand* out);
bool talkbackExpired(const RemoteCommand& cmd, bool wallValid, uint32_t epoch);
enum class TalkbackResult { Empty, Command, Failed };
// Returns only the command, never the response document or transport error.
// httpStatus (optional): HTTP status, or -1 when the transport failed.
TalkbackResult talkbackExecute(HttpClient& http, const char* base, uint32_t id,
                               const char* key, char* command, size_t cap,
                               int* httpStatus = nullptr);
}  // namespace gm
#endif
