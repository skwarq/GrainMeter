// SPDX-License-Identifier: MIT
#include "onewire/onewire.h"

namespace gm {

void OneWireBus::writeByte(uint8_t value) {
  for (int i = 0; i < 8; ++i) {
    writeBit((value >> i) & 0x01u);
  }
}

uint8_t OneWireBus::readByte() {
  uint8_t value = 0;
  for (int i = 0; i < 8; ++i) {
    if (readBit()) {
      value = static_cast<uint8_t>(value | (1u << i));
    }
  }
  return value;
}

static bool romBit(const Rom& rom, int bitIndex) {
  return (rom.b[bitIndex / 8] >> (bitIndex % 8)) & 0x01u;
}

static void setRomBit(Rom* rom, int bitIndex, bool value) {
  const uint8_t mask = static_cast<uint8_t>(1u << (bitIndex % 8));
  if (value) {
    rom->b[bitIndex / 8] = static_cast<uint8_t>(rom->b[bitIndex / 8] | mask);
  } else {
    rom->b[bitIndex / 8] = static_cast<uint8_t>(rom->b[bitIndex / 8] & ~mask);
  }
}

static void insertSorted(Rom* out, size_t* count, const Rom& rom) {
  for (size_t i = 0; i < *count; ++i) {
    if (out[i] == rom) {
      return;  // repeated path (noise); ignore duplicate
    }
  }
  size_t pos = *count;
  while (pos > 0 && rom < out[pos - 1]) {
    out[pos] = out[pos - 1];
    --pos;
  }
  out[pos] = rom;
  ++*count;
}

SearchResult searchRom(OneWireBus& bus, Rom* out, size_t cap) {
  SearchResult res = {false, false, false, 0, 0};
  Rom rom = {};
  int lastDiscrepancy = -1;  // bit index (0..63) of last branch taken as 0
  // Every pass discovers one leaf; bound the loop even with a noisy bus.
  const int kMaxPasses = 64;

  for (int pass = 0; pass < kMaxPasses; ++pass) {
    if (!bus.reset()) {
      if (pass == 0) {
        return res;  // nobody on the bus
      }
      res.busError = true;  // device vanished mid-enumeration
      return res;
    }
    res.presence = true;
    bus.writeByte(kCmdSearchRom);

    int lastZero = -1;
    for (int bit = 0; bit < 64; ++bit) {
      const bool idBit = bus.readBit();
      const bool cmpBit = bus.readBit();
      bool dir;
      if (idBit && cmpBit) {
        res.busError = true;  // no device participates any more
        return res;
      } else if (idBit != cmpBit) {
        dir = idBit;  // all remaining devices agree
      } else {
        // Discrepancy: devices with 0 and with 1 on this position.
        if (bit < lastDiscrepancy) {
          dir = romBit(rom, bit);
        } else {
          dir = (bit == lastDiscrepancy);
        }
        if (!dir) {
          lastZero = bit;
        }
      }
      setRomBit(&rom, bit, dir);
      bus.writeBit(dir);
    }

    if (rom.crcValid()) {
      if (res.found < cap) {
        insertSorted(out, &res.found, rom);
      } else {
        res.truncated = true;
      }
    } else {
      ++res.crcErrors;
    }

    lastDiscrepancy = lastZero;
    if (lastDiscrepancy < 0) {
      break;  // that was the last device
    }
  }
  return res;
}

}  // namespace gm
