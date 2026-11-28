// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace ow {

constexpr size_t kCapacity = 20000;
struct __attribute__((packed)) RawRecord {
  uint64_t duration_us;
  uint8_t level;
};
struct Snapshot {
  uint64_t begin;
  uint64_t end;
  uint64_t generation;
  uint64_t offset_us;
  uint64_t overwritten;
  uint64_t anomalies;
  uint64_t uptime_us;
  uint64_t recording_us;
  RawRecord tail;
};

esp_err_t captureStart();
void captureClear();
Snapshot captureSnapshot();
// A snapshot is invalidated by clear or by overwriting an unread record.
bool captureRead(const Snapshot& snapshot, uint64_t index, RawRecord* record);
bool captureValid(const Snapshot& snapshot);
void activityTask(void*);

}  // namespace ow
