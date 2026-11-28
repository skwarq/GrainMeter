// SPDX-License-Identifier: MIT
#ifndef GM_DS18B20_H
#define GM_DS18B20_H

#include <stdint.h>

#include "onewire/onewire.h"

namespace gm {

// Result of reading one sensor. A valid reading of 0.00 °C is Ok with
// centi == 0; every other status means "no temperature" and must never be
// reported as a number.
enum class ReadStatus : uint8_t {
  Ok = 0,
  Missing = 1,       // no presence pulse / scratchpad read as all 0xFF
  CrcError = 2,      // scratchpad CRC mismatch
  Unsupported = 3,   // family code not measured (e.g. 0x10, 0x22)
  PowerOnValue = 4,  // 85.00 °C power-on value: conversion did not run
  BusError = 5,      // line stuck low (scratchpad all zero)
  OutOfRange = 6,    // decoded value outside -55..+125 °C
  NotMapped = 7,     // point configured but no sensor mapped to it
};

const char* readStatusName(ReadStatus s);

enum { kScratchpadLen = 9, kConversionTimeMs12Bit = 750 };

// Decodes a raw scratchpad (9 bytes incl. CRC). On Ok, *centi holds the
// temperature in hundredths of °C (rounded from the 1/16 °C raw value,
// honouring the resolution bits in the configuration register).
ReadStatus decodeScratchpad(const uint8_t sp[kScratchpadLen], int16_t* centi);

// STW-100 point number (TH == TL, 1..kMaxPoints), or 0 when untrusted.
uint8_t pointFromScratchpad(const uint8_t sp[kScratchpadLen]);

// Reset + Skip ROM + Convert T for all devices. Returns presence.
bool startConversionAll(OneWireBus& bus);

// Polls read slots until the (externally powered) devices report completion
// or timeoutMs passes. Returns elapsed ms; *timedOut set accordingly.
uint32_t waitConversion(OneWireBus& bus, uint32_t timeoutMs, bool* timedOut);

// Match ROM + Read Scratchpad + decode. Unsupported families are rejected
// without touching the bus.
ReadStatus readSensor(OneWireBus& bus, const Rom& rom, int16_t* centi,
                      uint8_t rawOut[kScratchpadLen] = nullptr);

}  // namespace gm

#endif
