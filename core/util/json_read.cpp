// SPDX-License-Identifier: MIT
#include "util/json_read.h"

#include <string.h>

namespace gm {

namespace {

const char* ws(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  return p;
}

// Parses a string at p (which must be '"'). Copies to out when out != NULL.
// Returns pointer after the closing quote or NULL.
const char* readString(const char* p, char* out, size_t cap, bool* fits) {
  if (*p != '"') return nullptr;
  ++p;
  size_t n = 0;
  *fits = true;
  while (*p != '"') {
    char c = *p;
    if (c == '\0' || static_cast<unsigned char>(c) < 0x20) return nullptr;
    if (c == '\\') {
      ++p;
      switch (*p) {
        case '"':
          c = '"';
          break;
        case '\\':
          c = '\\';
          break;
        case '/':
          c = '/';
          break;
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case 'r':
          c = '\r';
          break;
        case 'b':
          c = '\b';
          break;
        case 'f':
          c = '\f';
          break;
        default:
          return nullptr;  // \u and invalid escapes
      }
    }
    if (out != nullptr) {
      if (n + 1 < cap) {
        out[n++] = c;
      } else {
        *fits = false;
      }
    }
    ++p;
  }
  if (out != nullptr && cap > 0) out[n] = '\0';
  return p + 1;
}

// Skips any JSON value. Returns pointer after it or NULL.
const char* skipValue(const char* p, int depth) {
  p = ws(p);
  if (depth > 8) return nullptr;
  if (*p == '"') {
    bool fits;
    return readString(p, nullptr, 0, &fits);
  }
  if (*p == '{' || *p == '[') {
    const char close = *p == '{' ? '}' : ']';
    const bool obj = *p == '{';
    p = ws(p + 1);
    if (*p == close) return p + 1;
    while (true) {
      if (obj) {
        bool fits;
        p = readString(ws(p), nullptr, 0, &fits);
        if (p == nullptr) return nullptr;
        p = ws(p);
        if (*p != ':') return nullptr;
        ++p;
      }
      p = skipValue(p, depth + 1);
      if (p == nullptr) return nullptr;
      p = ws(p);
      if (*p == ',') {
        ++p;
        continue;
      }
      if (*p == close) return p + 1;
      return nullptr;
    }
  }
  // number / true / false / null
  const char* start = p;
  while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
         *p != '\n' && *p != '\r' && *p != '\t') {
    ++p;
  }
  return p == start ? nullptr : p;
}

// Finds the value of a top-level key; returns pointer to it or NULL.
const char* findMember(const char* json, const char* key) {
  const char* p = ws(json);
  if (*p != '{') return nullptr;
  p = ws(p + 1);
  if (*p == '}') return nullptr;
  char name[48];
  while (true) {
    bool fits;
    p = readString(ws(p), name, sizeof(name), &fits);
    if (p == nullptr) return nullptr;
    p = ws(p);
    if (*p != ':') return nullptr;
    p = ws(p + 1);
    if (fits && strcmp(name, key) == 0) return p;
    p = skipValue(p, 1);
    if (p == nullptr) return nullptr;
    p = ws(p);
    if (*p == ',') {
      ++p;
      continue;
    }
    return nullptr;  // '}' or garbage: key not found
  }
}

}  // namespace

bool jsonGetString(const char* json, const char* key, char* out, size_t cap) {
  const char* v = findMember(json, key);
  if (v == nullptr || *v != '"') return false;
  bool fits = false;
  return readString(v, out, cap, &fits) != nullptr && fits;
}

bool jsonGetUint(const char* json, const char* key, uint32_t* out) {
  const char* v = findMember(json, key);
  if (v == nullptr || *v < '0' || *v > '9') return false;
  uint64_t n = 0;
  while (*v >= '0' && *v <= '9') {
    n = n * 10u + static_cast<uint64_t>(*v - '0');
    if (n > 0xFFFFFFFFull) return false;
    ++v;
  }
  if (*v == '.' || *v == 'e' || *v == 'E') return false;
  *out = static_cast<uint32_t>(n);
  return true;
}

}  // namespace gm
