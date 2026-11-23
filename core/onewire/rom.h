// SPDX-License-Identifier: MIT
#ifndef GM_ROM_H
#define GM_ROM_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// 64-bit 1-Wire ROM code. b[0] = family code, b[1..6] = serial (LSB first),
// b[7] = CRC-8 of b[0..6]. Text form: 16 hex chars in bus order, b[0]
// (family) first and b[7] (CRC) last, e.g. "28FF641E8516039A" — the family
// code is always the first two characters.
struct Rom {
  uint8_t b[8];

  uint8_t family() const { return b[0]; }
  bool crcValid() const;
  bool isZero() const;
  bool operator==(const Rom& o) const;
  bool operator!=(const Rom& o) const { return !(*this == o); }
  bool operator<(const Rom& o) const;  // ordering for stable listings
};

enum { kRomTextLen = 16 };  // without terminator

// Writes 16 hex chars + NUL into out (cap >= 17). Returns out.
const char* romToHex(const Rom& rom, char* out, size_t cap);

// Accepts 16 hex chars (upper/lower case). Separators ' ', ':', '-' are
// ignored. Does not check CRC (callers decide).
bool romFromHex(const char* text, Rom* out);

// Device family as found in 1-Wire ROM codes used by temperature probes.
enum class Family : uint8_t {
  DS18B20 = 0x28,  // DS18B20 or compatible (MAX31820 also uses 0x28)
  DS18S20 = 0x10,  // DS18S20 / DS1820
  DS1822 = 0x22,
};

// Human readable family name; never NULL.
const char* familyName(uint8_t family);

// Only 0x28 is measured in this version; others are listed in diagnostics.
inline bool familySupported(uint8_t family) { return family == 0x28; }

}  // namespace gm

#endif
