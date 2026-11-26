// SPDX-License-Identifier: MIT
#include "util/base64url.h"

#include <limits.h>

namespace gm {
namespace {
int value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}
}  // namespace

int base64urlDecode(const char* input, size_t len, uint8_t* out, size_t cap) {
  if (len && input == nullptr) return -1;
  size_t n = len;
  while (n && input[n - 1] == '=') --n;
  const size_t padding = len - n;
  const size_t tail = n % 4;
  if (tail == 1 || padding > 2 ||
      (padding && (len % 4 != 0 || padding != 4 - tail))) {
    return -1;
  }
  const size_t bytes = n / 4 * 3 + (tail ? tail - 1 : 0);
  if (bytes > cap || bytes > INT_MAX || (bytes && out == nullptr)) return -1;
  for (size_t i = 0; i < n; ++i) {
    if (value(input[i]) < 0) return -1;
  }
  if ((tail == 2 && (value(input[n - 1]) & 15)) ||
      (tail == 3 && (value(input[n - 1]) & 3))) {
    return -1;
  }
  unsigned bits = 0;
  unsigned count = 0;
  size_t written = 0;
  for (size_t i = 0; i < n; ++i) {
    bits = (bits << 6) | static_cast<unsigned>(value(input[i]));
    count += 6;
    if (count >= 8) {
      count -= 8;
      out[written++] = static_cast<uint8_t>(bits >> count);
    }
  }
  return static_cast<int>(written);
}
}  // namespace gm
