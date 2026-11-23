// SPDX-License-Identifier: MIT
#ifndef GM_HISTORY_LOG_H
#define GM_HISTORY_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "data/measurement.h"

namespace gm {

// Raw flash-like storage for the local measurement history. Erase sets a
// whole sector to 0xFF; write may only clear bits (NOR flash semantics).
// ESP32: the "history" data partition. PC: a file.
class HistoryStorage {
 public:
  virtual ~HistoryStorage() {}
  virtual size_t size() = 0;        // multiple of sectorSize()
  virtual size_t sectorSize() = 0;  // 4096 on the ESP32
  virtual bool read(size_t offset, void* buf, size_t len) = 0;
  virtual bool write(size_t offset, const void* buf, size_t len) = 0;
  virtual bool eraseSector(size_t offset) = 0;
};

// One stored measurement (32 bytes on flash, little endian).
struct HistoryRecord {
  uint32_t seq;                // increasing, survives restarts
  uint32_t epoch;              // UTC seconds (records need a valid clock)
  int16_t centi[kMaxPoints];   // kHistoryMissing = no value
  uint8_t status[kMaxPoints];  // ReadStatus per point
};

enum : int16_t { kHistoryMissing = INT16_MIN };
enum { kHistoryRecordSize = 32 };

// Append-only circular log. Sectors are filled in turn; when the log is
// full the oldest sector is erased. Each record carries a CRC-8, so a
// record torn by a power loss is skipped. Wear: every sector is erased once
// per (sectors x records-per-sector) appends, e.g. every ~64 days for
// 192 KiB at one record per 15 min.
class HistoryLog {
 public:
  HistoryLog() : st_(nullptr), next_(0), nextSeq_(1), count_(0), ok_(false) {}

  // Scans the storage to find the write position. Returns false if the
  // storage is unusable (history is then disabled, the device works on).
  bool begin(HistoryStorage* storage);
  bool available() const { return ok_; }

  bool append(uint32_t epoch, const int16_t centi[kMaxPoints],
              const uint8_t status[kMaxPoints]);

  size_t count() const { return count_; }
  size_t capacity() const;
  uint32_t oldestEpoch() const { return oldestEpoch_; }

  // Calls fn for every valid record with from <= epoch <= to, oldest
  // first. Returns false on a read error.
  typedef void (*Visitor)(void* ctx, const HistoryRecord& r);
  bool forEach(uint32_t from, uint32_t to, Visitor fn, void* ctx);

  // Encode/decode of the 32-byte flash format (exposed for tests).
  static void encode(const HistoryRecord& r, uint8_t out[kHistoryRecordSize]);
  static bool decode(const uint8_t in[kHistoryRecordSize], HistoryRecord* r);

 private:
  size_t sectors() const;
  size_t perSector() const;

  HistoryStorage* st_;
  size_t next_;  // byte offset of the next free slot
  uint32_t nextSeq_;
  size_t count_;
  uint32_t oldestEpoch_ = 0;
  bool ok_;
};

// Compact binary export for the portal (GET /api/history/<range>):
//   header: "GMH1", uint16 rows, uint16 points(8)
//   rows:   uint32 epoch, int16 centi[8] (INT16_MIN = missing)
// Rows are averaged into equal time buckets so that at most maxRows are
// returned (720 rows = 14.4 KiB, fits the device's reply buffer).
enum { kHistoryHeaderSize = 8, kHistoryRowSize = 20 };

size_t historyExport(HistoryLog& log, uint32_t from, uint32_t to,
                     size_t maxRows, uint8_t* out, size_t cap);

}  // namespace gm

#endif
