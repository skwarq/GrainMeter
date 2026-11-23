// SPDX-License-Identifier: MIT
#ifndef GM_MEASUREMENT_H
#define GM_MEASUREMENT_H

#include <stddef.h>
#include <stdint.h>

#include "onewire/ds18b20.h"
#include "onewire/rom.h"

namespace gm {

enum {
  kMaxPoints = 8,    // ThingSpeak channel has 8 fields -> 8 measuring points
  kMaxSensors = 16,  // devices tracked on the bus (scan/diagnostics)
};

struct SensorReading {
  Rom rom;
  ReadStatus status;
  int16_t centi;  // valid only when status == Ok
  uint8_t point;  // 1..kMaxPoints, 0 = not mapped
};

struct ScanReport {
  SearchResult search;
  Rom roms[kMaxSensors];
  uint32_t monoS;  // when the scan ran
};

struct MeasureReport {
  bool presence;
  bool conversionTimeout;
  uint32_t conversionMs;
  uint32_t monoS;
  uint32_t epoch;  // 0 when wall clock was not valid
  size_t count;
  SensorReading readings[kMaxSensors];
  size_t crcRomErrors;  // from the search that preceded the reading
};

// One row stored in the sample buffer and uploaded as one ThingSpeak entry.
// Plain-old-data, kept in RTC memory on the ESP32.
struct Sample {
  uint32_t seq;    // monotonically increasing, never reused
  uint32_t epoch;  // UTC seconds; 0 if wall clock unknown at capture
  uint32_t monoS;  // monotonic seconds since power-on (RTC timer)
  int16_t centi[kMaxPoints];
  uint8_t status[kMaxPoints];  // ReadStatus per point
};

inline bool sampleHasValue(const Sample& s, int pointIndex) {
  return s.status[pointIndex] == static_cast<uint8_t>(ReadStatus::Ok);
}

}  // namespace gm

#endif
