// SPDX-License-Identifier: MIT
#ifndef GM_ROMMAP_H
#define GM_ROMMAP_H

#include <stddef.h>
#include <stdint.h>

#include "data/measurement.h"
#include "onewire/rom.h"

namespace gm {

// Persistent assignment ROM -> measuring point (ThingSpeak field) and the
// sensor height above the probe tip. Points are unique; ROM codes are unique.
class RomMap {
 public:
  struct Entry {
    Rom rom;
    uint8_t point;     // 1..kMaxPoints
    int16_t heightCm;  // informational; -1 = unknown
  };
  enum { kCapacity = kMaxPoints };

  RomMap() : count_(0) {}

  void clear() { count_ = 0; }
  size_t size() const { return count_; }
  const Entry& at(size_t i) const { return entries_[i]; }

  // Assigns rom to point. Any previous mapping of the same ROM or the same
  // point is replaced. Fails for invalid point / CRC / unsupported family.
  bool assign(const Rom& rom, uint8_t point, int16_t heightCm,
              const char** err);
  bool remove(const Rom& rom);
  uint8_t pointOf(const Rom& rom) const;  // 0 = not mapped
  const Entry* findPoint(uint8_t point) const;

  // Assigns unmapped supported ROMs (in ascending ROM order) to free points.
  // Returns number of new assignments.
  size_t autoAssign(const Rom* roms, size_t n);

  // Text form persisted in NVS / config file:
  //   "<ROMHEX>:<point>:<height>;<ROMHEX>:<point>:<height>"
  // Returns false (and leaves *this untouched) on any syntax error.
  bool serialize(char* out, size_t cap) const;
  bool parse(const char* text);

 private:
  Entry entries_[kCapacity];
  size_t count_;
};

}  // namespace gm

#endif
