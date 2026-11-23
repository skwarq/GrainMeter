// SPDX-License-Identifier: MIT
#include "onewire/rom.h"

#include "util/crc.h"

namespace gm {

bool Rom::crcValid() const { return !isZero() && crc8Maxim(b, 7) == b[7]; }

bool Rom::isZero() const {
  for (int i = 0; i < 8; ++i) {
    if (b[i] != 0) {
      return false;
    }
  }
  return true;
}

bool Rom::operator==(const Rom& o) const {
  for (int i = 0; i < 8; ++i) {
    if (b[i] != o.b[i]) {
      return false;
    }
  }
  return true;
}

bool Rom::operator<(const Rom& o) const {
  for (int i = 0; i < 8; ++i) {
    if (b[i] != o.b[i]) {
      return b[i] < o.b[i];
    }
  }
  return false;
}

const char* romToHex(const Rom& rom, char* out, size_t cap) {
  static const char kHex[] = "0123456789ABCDEF";
  if (cap < kRomTextLen + 1) {
    if (cap > 0) {
      out[0] = '\0';
    }
    return out;
  }
  for (int i = 0; i < 8; ++i) {
    out[2 * i] = kHex[rom.b[i] >> 4];
    out[2 * i + 1] = kHex[rom.b[i] & 0x0F];
  }
  out[kRomTextLen] = '\0';
  return out;
}

static int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool romFromHex(const char* text, Rom* out) {
  if (text == nullptr || out == nullptr) {
    return false;
  }
  Rom rom = {};
  int nibbles = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == ' ' || *p == ':' || *p == '-') {
      continue;
    }
    const int v = hexValue(*p);
    if (v < 0 || nibbles >= 16) {
      return false;
    }
    const int idx = nibbles / 2;
    rom.b[idx] = static_cast<uint8_t>((rom.b[idx] << 4) | v);
    ++nibbles;
  }
  if (nibbles != 16) {
    return false;
  }
  *out = rom;
  return true;
}

const char* familyName(uint8_t family) {
  switch (family) {
    case 0x28:
      return "DS18B20";
    case 0x10:
      return "DS18S20/DS1820";
    case 0x22:
      return "DS1822";
    default:
      return "unknown";
  }
}

}  // namespace gm
