// SPDX-License-Identifier: MIT
#ifndef GM_FORM_H
#define GM_FORM_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// application/x-www-form-urlencoded access over a bounded body.
class Form {
 public:
  Form(const char* body, size_t len) : body_(body), len_(len) {}

  // Copies the decoded value of `name` into out (NUL terminated).
  // Returns false if missing, malformed or longer than cap-1.
  bool get(const char* name, char* out, size_t cap) const;
  bool has(const char* name) const;
  // Parses a decimal integer within [lo, hi].
  bool getInt(const char* name, long lo, long hi, long* out) const;

 private:
  const char* body_;
  size_t len_;
};

// Percent-decodes src[0..len) into out; '+' becomes space. Returns false on
// malformed escapes, embedded NUL or overflow.
bool urlDecode(const char* src, size_t len, char* out, size_t cap);

// Constant-time comparison for secrets (length leak only).
bool secretEquals(const char* a, const char* b);

}  // namespace gm

#endif
