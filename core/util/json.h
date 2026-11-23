// SPDX-License-Identifier: MIT
#ifndef GM_JSON_H
#define GM_JSON_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// Minimal streaming JSON writer into a caller-provided fixed buffer.
// Overflow is sticky: once the buffer is too small, ok() returns false and
// the output must be discarded. Commas are inserted automatically.
class JsonWriter {
 public:
  JsonWriter(char* buf, size_t cap);

  JsonWriter& beginObject();
  JsonWriter& endObject();
  JsonWriter& beginArray();
  JsonWriter& endArray();
  JsonWriter& key(const char* k);

  JsonWriter& str(const char* v);  // NULL -> null
  JsonWriter& num(int64_t v);
  JsonWriter& unum(uint64_t v);
  JsonWriter& boolean(bool v);
  JsonWriter& null();
  // Fixed-point decimal, e.g. centi(2153) -> 21.53
  JsonWriter& centi(int32_t v);
  JsonWriter& raw(const char* fragment);  // trusted, already valid JSON

  // key + value shortcuts
  JsonWriter& kv(const char* k, const char* v) { return key(k).str(v); }
  JsonWriter& kv(const char* k, int64_t v) { return key(k).num(v); }
  JsonWriter& kvu(const char* k, uint64_t v) { return key(k).unum(v); }
  JsonWriter& kvb(const char* k, bool v) { return key(k).boolean(v); }

  bool ok() const { return ok_; }
  size_t length() const { return len_; }
  const char* c_str() const { return buf_; }

 private:
  void put(char c);
  void puts(const char* s);
  void sep();

  char* buf_;
  size_t cap_;
  size_t len_;
  bool ok_;
  bool needComma_;
  bool afterKey_;
};

}  // namespace gm

#endif
