// SPDX-License-Identifier: MIT
#include "onewire/ds18b20.h"

#include "data/measurement.h"
#include "util/crc.h"

namespace gm {

const char* readStatusName(ReadStatus s) {
  switch (s) {
    case ReadStatus::Ok:
      return "ok";
    case ReadStatus::Missing:
      return "missing";
    case ReadStatus::CrcError:
      return "crc_error";
    case ReadStatus::Unsupported:
      return "unsupported_family";
    case ReadStatus::PowerOnValue:
      return "power_on_value";
    case ReadStatus::BusError:
      return "bus_error";
    case ReadStatus::OutOfRange:
      return "out_of_range";
    case ReadStatus::NotMapped:
      return "not_mapped";
  }
  return "unknown";
}

ReadStatus decodeScratchpad(const uint8_t sp[kScratchpadLen], int16_t* centi) {
  bool allFF = true;
  bool allZero = true;
  for (int i = 0; i < kScratchpadLen; ++i) {
    allFF = allFF && sp[i] == 0xFF;
    allZero = allZero && sp[i] == 0x00;
  }
  if (allFF) {
    return ReadStatus::Missing;  // nobody drove the line
  }
  if (allZero) {
    return ReadStatus::BusError;  // CRC of zeros is 0, so check first
  }
  if (crc8Maxim(sp, 8) != sp[8]) {
    return ReadStatus::CrcError;
  }
  int16_t raw = static_cast<int16_t>(static_cast<uint16_t>(sp[1] << 8) | sp[0]);
  // Undefined low bits at reduced resolution (config register bits 6:5).
  const int resolution = (sp[4] >> 5) & 0x03;  // 0=9 bit .. 3=12 bit
  const int16_t masks[4] = {static_cast<int16_t>(~7), static_cast<int16_t>(~3),
                            static_cast<int16_t>(~1), static_cast<int16_t>(~0)};
  raw = static_cast<int16_t>(raw & masks[resolution]);
  if (raw == 0x0550) {
    // 85.0 °C is the power-on register value. Grain never reaches it, so
    // treat it as "conversion not performed" rather than a measurement.
    return ReadStatus::PowerOnValue;
  }
  if (raw < -55 * 16 || raw > 125 * 16) {
    return ReadStatus::OutOfRange;
  }
  const int32_t scaled = static_cast<int32_t>(raw) * 100;
  const int32_t rounded = scaled >= 0 ? (scaled + 8) / 16 : (scaled - 8) / 16;
  *centi = static_cast<int16_t>(rounded);
  return ReadStatus::Ok;
}

uint8_t pointFromScratchpad(const uint8_t sp[kScratchpadLen]) {
  int16_t centi = 0;
  const ReadStatus status = decodeScratchpad(sp, &centi);
  if (status != ReadStatus::Ok && status != ReadStatus::PowerOnValue &&
      status != ReadStatus::OutOfRange) {
    return 0;
  }
  return sp[2] == sp[3] && sp[2] >= 1 && sp[2] <= kMaxPoints ? sp[2] : 0;
}

bool startConversionAll(OneWireBus& bus) {
  if (!bus.reset()) {
    return false;
  }
  bus.writeByte(kCmdSkipRom);
  bus.writeByte(kCmdConvertT);
  return true;
}

uint32_t waitConversion(OneWireBus& bus, uint32_t timeoutMs, bool* timedOut) {
  const uint32_t kStepMs = 10;
  uint32_t elapsed = 0;
  while (elapsed < timeoutMs) {
    if (bus.readBit()) {
      *timedOut = false;
      return elapsed;
    }
    bus.sleepMs(kStepMs);
    elapsed += kStepMs;
  }
  *timedOut = !bus.readBit();
  return elapsed;
}

ReadStatus readSensor(OneWireBus& bus, const Rom& rom, int16_t* centi,
                      uint8_t rawOut[kScratchpadLen]) {
  if (!familySupported(rom.family())) {
    return ReadStatus::Unsupported;
  }
  if (!bus.reset()) {
    return ReadStatus::Missing;
  }
  bus.writeByte(kCmdMatchRom);
  for (int i = 0; i < 8; ++i) {
    bus.writeByte(rom.b[i]);
  }
  bus.writeByte(kCmdReadScratchpad);
  uint8_t sp[kScratchpadLen];
  for (int i = 0; i < kScratchpadLen; ++i) {
    sp[i] = bus.readByte();
  }
  if (rawOut != nullptr) {
    for (int i = 0; i < kScratchpadLen; ++i) {
      rawOut[i] = sp[i];
    }
  }
  return decodeScratchpad(sp, centi);
}

}  // namespace gm
