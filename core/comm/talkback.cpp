// SPDX-License-Identifier: MIT
#include "comm/talkback.h"

#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "util/json_read.h"
namespace gm {
namespace {
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
const char* skip(const char* p) {
  while (space(*p)) ++p;
  return p;
}
// Validate the complete response before using the small member reader,
// which intentionally stops as soon as it finds the requested member.
const char* jsonStringEnd(const char* p) {
  if (*p++ != '"') return nullptr;
  while (*p && *p != '"') {
    if (static_cast<unsigned char>(*p) < 32) return nullptr;
    if (*p++ == '\\') {
      if (!*p) return nullptr;
      if (*p == 'u') {
        ++p;
        for (unsigned i = 0; i < 4; ++i, ++p)
          if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                (*p >= 'A' && *p <= 'F')))
            return nullptr;
      } else {
        if (!strchr("\"\\/bfnrt", *p)) return nullptr;
        ++p;
      }
    }
  }
  return *p == '"' ? p + 1 : nullptr;
}
const char* jsonValueEnd(const char* p, unsigned depth) {
  if (depth > 8) return nullptr;
  p = skip(p);
  if (*p == '"') return jsonStringEnd(p);
  if (*p == '{' || *p == '[') {
    const bool object = *p == '{';
    const char close = object ? '}' : ']';
    p = skip(p + 1);
    if (*p == close) return p + 1;
    while (true) {
      if (object) {
        p = jsonStringEnd(p);
        if (!p || *skip(p) != ':') return nullptr;
        p = skip(p) + 1;
      }
      p = jsonValueEnd(p, depth + 1);
      if (!p) return nullptr;
      p = skip(p);
      if (*p == close) return p + 1;
      if (*p != ',') return nullptr;
      p = skip(p + 1);
    }
  }
  for (const char* literal : {"true", "false", "null"}) {
    const size_t n = strlen(literal);
    if (!strncmp(p, literal, n)) return p + n;
  }
  if (*p == '-') ++p;
  if (*p == '0')
    ++p;
  else {
    if (*p < '1' || *p > '9') return nullptr;
    do {
      ++p;
    } while (*p >= '0' && *p <= '9');
  }
  if (*p == '.') {
    ++p;
    if (*p < '0' || *p > '9') return nullptr;
    do {
      ++p;
    } while (*p >= '0' && *p <= '9');
  }
  if (*p == 'e' || *p == 'E') {
    ++p;
    if (*p == '+' || *p == '-') ++p;
    if (*p < '0' || *p > '9') return nullptr;
    do {
      ++p;
    } while (*p >= '0' && *p <= '9');
  }
  return p;
}
bool number(const char*& p, uint32_t* out) {
  if (*p < '0' || *p > '9') return false;
  uint32_t v = 0;
  while (*p >= '0' && *p <= '9') {
    const unsigned d = static_cast<unsigned>(*p++ - '0');
    if (v > (UINT32_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  *out = v;
  return true;
}
}  // namespace
bool talkbackParseCommand(const char* text, RemoteCommand* out) {
  if (!text || !out) return false;
  const char* p = skip(text);
  if (strncmp(p, "REMOTE", 6) || !space(p[6])) return false;
  p = skip(p + 6);
  RemoteCommand c = {};
  if (!number(p, &c.minutes) || c.minutes < 1 || c.minutes > 60 || !space(*p))
    return false;
  p = skip(p);
  if (!number(p, &c.validUntil) || *skip(p)) return false;
  *out = c;
  return true;
}
bool talkbackExpired(const RemoteCommand& cmd, bool wallValid, uint32_t epoch) {
  return wallValid && cmd.validUntil && epoch >= cmd.validUntil;
}
TalkbackResult talkbackExecute(HttpClient& http, const char* base, uint32_t id,
                               const char* key, char* command, size_t cap,
                               int* httpStatus) {
  command[0] = 0;
  if (httpStatus) *httpStatus = -1;
  char url[160], body[240], response[1024];
  const int n =
      snprintf(url, sizeof(url), "%s/talkbacks/%lu/commands/execute.json", base,
               static_cast<unsigned long>(id));
  JsonWriter w(body, sizeof(body));
  w.beginObject().kv("api_key", key).endObject();
  if (n < 0 || static_cast<size_t>(n) >= sizeof(url) || !w.ok())
    return TalkbackResult::Failed;
  response[0] = 0;
  const HttpResult r =
      http.postJson(url, body, w.length(), 3000, response, sizeof(response));
  if (httpStatus && r.transportOk) *httpStatus = r.status;
  if (!r.transportOk || r.status != 200) return TalkbackResult::Failed;
  if (!*skip(response)) return TalkbackResult::Empty;
  const char* end =
      *skip(response) == '{' ? jsonValueEnd(response, 0) : nullptr;
  if (!end || *skip(end)) return TalkbackResult::Failed;
  // ThingSpeak answers an empty queue with "{}" (an object without a command).
  if (!strstr(response, "\"command_string\"")) return TalkbackResult::Empty;
  if (!jsonGetString(response, "command_string", command, cap))
    return TalkbackResult::Failed;
  return *command ? TalkbackResult::Command : TalkbackResult::Empty;
}
}  // namespace gm
