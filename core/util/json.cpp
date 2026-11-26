// SPDX-License-Identifier: MIT
#include "util/json.h"

#include <inttypes.h>
#include <stdio.h>

namespace gm {

JsonWriter::JsonWriter(char* buf, size_t cap)
    : buf_(buf),
      cap_(cap),
      len_(0),
      ok_(cap > 0),
      needComma_(false),
      afterKey_(false) {
  if (cap_ > 0) {
    buf_[0] = '\0';
  }
}

void JsonWriter::put(char c) {
  if (!ok_) {
    return;
  }
  if (len_ + 1 >= cap_) {
    ok_ = false;
    return;
  }
  buf_[len_++] = c;
  buf_[len_] = '\0';
}

void JsonWriter::puts(const char* s) {
  while (*s != '\0') {
    put(*s++);
  }
}

void JsonWriter::sep() {
  if (afterKey_) {
    afterKey_ = false;
    return;
  }
  if (needComma_) {
    put(',');
  }
}

JsonWriter& JsonWriter::beginObject() {
  sep();
  put('{');
  needComma_ = false;
  return *this;
}

JsonWriter& JsonWriter::endObject() {
  put('}');
  needComma_ = true;
  return *this;
}

JsonWriter& JsonWriter::beginArray() {
  sep();
  put('[');
  needComma_ = false;
  return *this;
}

JsonWriter& JsonWriter::endArray() {
  put(']');
  needComma_ = true;
  return *this;
}

JsonWriter& JsonWriter::key(const char* k) {
  sep();
  afterKey_ = true;  // str() must not add another separator
  str(k);
  put(':');
  needComma_ = false;
  afterKey_ = true;
  return *this;
}

JsonWriter& JsonWriter::str(const char* v) {
  if (v == nullptr) {
    return null();
  }
  sep();
  put('"');
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(v); *p;
       ++p) {
    switch (*p) {
      case '"':
        puts("\\\"");
        break;
      case '\\':
        puts("\\\\");
        break;
      case '\n':
        puts("\\n");
        break;
      case '\r':
        puts("\\r");
        break;
      case '\t':
        puts("\\t");
        break;
      case '\b':
        puts("\\b");
        break;
      case '\f':
        puts("\\f");
        break;
      default:
        if (*p < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", *p);
          puts(esc);
        } else {
          put(static_cast<char>(*p));
        }
    }
  }
  put('"');
  needComma_ = true;
  return *this;
}

JsonWriter& JsonWriter::num(int64_t v) {
  char tmp[24];
  snprintf(tmp, sizeof(tmp), "%" PRId64, v);
  return raw(tmp);
}

JsonWriter& JsonWriter::unum(uint64_t v) {
  char tmp[24];
  snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
  return raw(tmp);
}

JsonWriter& JsonWriter::boolean(bool v) { return raw(v ? "true" : "false"); }

JsonWriter& JsonWriter::null() { return raw("null"); }

JsonWriter& JsonWriter::centi(int32_t v) {
  char tmp[24];
  const bool neg = v < 0;
  const uint32_t a = neg ? static_cast<uint32_t>(-static_cast<int64_t>(v))
                         : static_cast<uint32_t>(v);
  snprintf(tmp, sizeof(tmp), "%s%" PRIu32 ".%02" PRIu32, neg ? "-" : "",
           a / 100u, a % 100u);
  return raw(tmp);
}

JsonWriter& JsonWriter::raw(const char* fragment) {
  sep();
  puts(fragment);
  needComma_ = true;
  return *this;
}

}  // namespace gm
