// SPDX-License-Identifier: MIT
#include "util/form.h"

#include <stdlib.h>
#include <string.h>

namespace gm {

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool urlDecode(const char* src, size_t len, char* out, size_t cap) {
  if (cap == 0) {
    return false;
  }
  size_t o = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = src[i];
    if (c == '+') {
      c = ' ';
    } else if (c == '%') {
      if (i + 2 >= len) {
        return false;  // truncated escape
      }
      const int hi = hexNibble(src[i + 1]);
      const int lo = hexNibble(src[i + 2]);
      if (hi < 0 || lo < 0) {
        return false;
      }
      c = static_cast<char>((hi << 4) | lo);
      i += 2;
    }
    if (c == '\0' || o + 1 >= cap) {
      return false;
    }
    out[o++] = c;
  }
  out[o] = '\0';
  return true;
}

static bool findField(const char* body, size_t len, const char* name,
                      const char** val, size_t* valLen) {
  const size_t nameLen = strlen(name);
  size_t pos = 0;
  while (pos < len) {
    size_t end = pos;
    while (end < len && body[end] != '&') {
      ++end;
    }
    size_t eq = pos;
    while (eq < end && body[eq] != '=') {
      ++eq;
    }
    if (eq - pos == nameLen && memcmp(body + pos, name, nameLen) == 0) {
      if (eq < end) {
        *val = body + eq + 1;
        *valLen = end - eq - 1;
      } else {
        *val = body + end;
        *valLen = 0;
      }
      return true;
    }
    pos = end + 1;
  }
  return false;
}

bool Form::get(const char* name, char* out, size_t cap) const {
  const char* v = nullptr;
  size_t vl = 0;
  if (!findField(body_, len_, name, &v, &vl)) {
    return false;
  }
  return urlDecode(v, vl, out, cap);
}

bool Form::has(const char* name) const {
  const char* v = nullptr;
  size_t vl = 0;
  return findField(body_, len_, name, &v, &vl);
}

bool Form::getInt(const char* name, long lo, long hi, long* out) const {
  char tmp[24];
  if (!get(name, tmp, sizeof(tmp)) || tmp[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const long v = strtol(tmp, &end, 10);
  if (*end != '\0' || v < lo || v > hi) {
    return false;
  }
  *out = v;
  return true;
}

bool secretEquals(const char* a, const char* b) {
  const size_t la = strlen(a);
  const size_t lb = strlen(b);
  unsigned diff = static_cast<unsigned>(la ^ lb);
  const size_t n = la < lb ? la : lb;
  for (size_t i = 0; i < n; ++i) {
    diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^
                                  static_cast<unsigned char>(b[i]));
  }
  return diff == 0 && la > 0;
}

}  // namespace gm
