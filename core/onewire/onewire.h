// SPDX-License-Identifier: MIT
#ifndef GM_ONEWIRE_H
#define GM_ONEWIRE_H

#include <stddef.h>
#include <stdint.h>

#include "onewire/rom.h"

namespace gm {

// Hardware boundary for the 1-Wire bus. Implementations provide time slots
// only; ROM search, addressing, CRC checks and DS18B20 handling are shared.
//  - ESP32: open-drain GPIO bit-banging (firmware/main/onewire_gpio.cpp)
//  - PC:    bit-level simulation of a probe (pc/sim_probe.cpp)
class OneWireBus {
 public:
  virtual ~OneWireBus() {}
  // Reset pulse; returns true when at least one device answered with a
  // presence pulse.
  virtual bool reset() = 0;
  virtual void writeBit(bool bit) = 0;
  virtual bool readBit() = 0;
  // Blocking wait used while temperature conversion is running.
  virtual void sleepMs(uint32_t ms) = 0;

  void writeByte(uint8_t value);
  uint8_t readByte();
};

enum OneWireCommand : uint8_t {
  kCmdSearchRom = 0xF0,
  kCmdMatchRom = 0x55,
  kCmdSkipRom = 0xCC,
  kCmdConvertT = 0x44,
  kCmdReadScratchpad = 0xBE,
};

struct SearchResult {
  bool presence;     // any presence pulse on the first reset
  bool busError;     // both bit and complement read as 1 mid-search
  bool truncated;    // more valid devices than output capacity
  size_t found;      // number of valid ROMs written to output
  size_t crcErrors;  // ROMs discovered with an invalid CRC (discarded)
};

// Search ROM (0xF0) following Maxim application note 187. Writes up to cap
// CRC-valid ROM codes, sorted ascending, without duplicates.
SearchResult searchRom(OneWireBus& bus, Rom* out, size_t cap);

}  // namespace gm

#endif
