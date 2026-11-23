// SPDX-License-Identifier: MIT
#include "data/rommap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gm {

bool RomMap::assign(const Rom& rom, uint8_t point, int16_t heightCm,
                    const char** err) {
  const char* dummy = nullptr;
  if (err == nullptr) {
    err = &dummy;
  }
  if (point < 1 || point > kMaxPoints) {
    *err = "point must be 1..8";
    return false;
  }
  if (!rom.crcValid()) {
    *err = "ROM CRC invalid";
    return false;
  }
  if (!familySupported(rom.family())) {
    *err = "unsupported sensor family";
    return false;
  }
  if (heightCm < -1) {
    *err = "height must be >= 0 cm (or -1 = unknown)";
    return false;
  }
  // Drop previous mapping for this ROM or this point.
  size_t w = 0;
  for (size_t r = 0; r < count_; ++r) {
    if (entries_[r].rom == rom || entries_[r].point == point) {
      continue;
    }
    entries_[w++] = entries_[r];
  }
  count_ = w;
  if (count_ >= kCapacity) {
    *err = "mapping table full";
    return false;
  }
  Entry e;
  e.rom = rom;
  e.point = point;
  e.heightCm = heightCm;
  // Keep sorted by point for stable output.
  size_t pos = count_;
  while (pos > 0 && entries_[pos - 1].point > point) {
    entries_[pos] = entries_[pos - 1];
    --pos;
  }
  entries_[pos] = e;
  ++count_;
  return true;
}

bool RomMap::remove(const Rom& rom) {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].rom == rom) {
      for (size_t j = i + 1; j < count_; ++j) {
        entries_[j - 1] = entries_[j];
      }
      --count_;
      return true;
    }
  }
  return false;
}

uint8_t RomMap::pointOf(const Rom& rom) const {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].rom == rom) {
      return entries_[i].point;
    }
  }
  return 0;
}

const RomMap::Entry* RomMap::findPoint(uint8_t point) const {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].point == point) {
      return &entries_[i];
    }
  }
  return nullptr;
}

size_t RomMap::autoAssign(const Rom* roms, size_t n) {
  size_t added = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!familySupported(roms[i].family()) || !roms[i].crcValid() ||
        pointOf(roms[i]) != 0) {
      continue;
    }
    uint8_t freePoint = 0;
    for (uint8_t p = 1; p <= kMaxPoints; ++p) {
      if (findPoint(p) == nullptr) {
        freePoint = p;
        break;
      }
    }
    if (freePoint == 0) {
      break;
    }
    if (assign(roms[i], freePoint, -1, nullptr)) {
      ++added;
    }
  }
  return added;
}

bool RomMap::serialize(char* out, size_t cap) const {
  if (cap == 0) {
    return false;
  }
  size_t used = 0;
  out[0] = '\0';
  for (size_t i = 0; i < count_; ++i) {
    char hex[kRomTextLen + 1];
    romToHex(entries_[i].rom, hex, sizeof(hex));
    const int n = snprintf(out + used, cap - used, "%s%s:%u:%d", i ? ";" : "",
                           hex, static_cast<unsigned>(entries_[i].point),
                           entries_[i].heightCm);
    if (n < 0 || static_cast<size_t>(n) >= cap - used) {
      out[0] = '\0';
      return false;
    }
    used += static_cast<size_t>(n);
  }
  return true;
}

bool RomMap::parse(const char* text) {
  RomMap tmp;
  if (text == nullptr) {
    return false;
  }
  const char* p = text;
  while (*p != '\0') {
    // ROM
    char hex[kRomTextLen + 1];
    size_t n = 0;
    while (*p != '\0' && *p != ':' && n < kRomTextLen) {
      hex[n++] = *p++;
    }
    hex[n] = '\0';
    if (*p != ':') {
      return false;
    }
    ++p;
    Rom rom;
    if (!romFromHex(hex, &rom)) {
      return false;
    }
    char* end = nullptr;
    const long point = strtol(p, &end, 10);
    if (end == p || *end != ':') {
      return false;
    }
    p = end + 1;
    const long height = strtol(p, &end, 10);
    if (end == p || (*end != ';' && *end != '\0') || height < -1 ||
        height > 32767) {
      return false;
    }
    p = (*end == ';') ? end + 1 : end;
    if (point < 1 || point > kMaxPoints) {
      return false;
    }
    if (!tmp.assign(rom, static_cast<uint8_t>(point),
                    static_cast<int16_t>(height), nullptr)) {
      return false;
    }
  }
  *this = tmp;
  return true;
}

}  // namespace gm
