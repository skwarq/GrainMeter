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

// Device state stored with the measurements (HistoryRecord::flags).
enum HistoryFlags : uint8_t {
  kHistPowerMask = 0x03,  // 0 normal, 1 economy, 2 critical, 3 unknown
  kHistUploadShift = 2,   // bits 2-3: last upload 0 none, 1 ok, 2 failed
  kHistUploadMask = 0x0C,
  kHistReconstructed = 0x10,  // time reconstructed after the first sync
  kHistRestart = 0x20,        // first measurement after a power-up
  kHistHold = 0x40,           // export only: last state held until now
};

// Full device state at one point in time.
struct HistoryRecord {
  uint32_t epoch;              // UTC seconds (records need a valid clock)
  int16_t centi[kMaxPoints];   // kHistoryMissing = no value
  uint8_t status[kMaxPoints];  // ReadStatus per point
  uint16_t batteryMv;          // 0 = unknown (stored with 10 mV steps)
  int8_t rssi;                 // dBm of the last Wi-Fi connection, 0 = unknown
  uint8_t flags;               // HistoryFlags
};

enum : int16_t { kHistoryMissing = INT16_MIN };

// When a measurement is worth storing. Temperatures in a silo change slowly:
// a measurement equal to the last stored one (within the dead bands) is not
// written; the heartbeat still writes one record per heartbeatS so that a
// missing device can be told from an unchanged silo.
struct HistoryPolicy {
  // 0.15 degC: above the +-1 LSB noise of the DS18B20 (0.0625 degC steps)
  uint16_t deadbandCenti = 15;
  uint16_t batteryDeadbandMv = 30;
  uint8_t rssiDeadbandDb = 6;
  uint32_t heartbeatS = 3u * 3600u;
};

struct HistoryStats {
  size_t records;
  size_t usedBytes;
  size_t capacityBytes;
  uint32_t oldestEpoch;  // 0 = empty
  uint32_t newestEpoch;
};

// Append-only circular log of variable-length records on NOR flash.
//
// Every sector starts with a key record (full state + sector sequence
// number); the following records store only what changed against the
// previous record (time step, changed points as small varint deltas,
// changed device state). A typical record is 6-12 bytes instead of 32, and
// unchanged measurements are not stored at all. When the log is full the
// oldest sector is erased; each sector decodes on its own.
//
// Every record ends with a CRC-8: a record torn by a power loss is skipped
// (reader and writer both continue from the last valid state).
class HistoryLog {
 public:
  HistoryLog() {}

  // Scans the sector heads and replays the newest sector (a few KiB of
  // reads, done on every wake). Returns false if the storage is unusable
  // (history is then disabled, the device works on).
  bool begin(HistoryStorage* storage);
  bool available() const { return ok_; }
  void setPolicy(const HistoryPolicy& p) { policy_ = p; }
  const HistoryPolicy& policy() const { return policy_; }

  // Offers one measurement. It is stored only if it differs from the last
  // stored state (see HistoryPolicy) or carries kHistRestart. prevEpoch =
  // time of the previous measurement (0 = unknown): when a change ends a
  // plateau of skipped measurements, the held state is first stored at
  // prevEpoch so a line drawn between records stays within the dead band.
  // Returns false only on a flash error; *stored tells if anything was
  // written.
  bool append(const HistoryRecord& r, uint32_t prevEpoch,
              bool* stored = nullptr);

  // Last stored state (false if the log is empty).
  bool last(HistoryRecord* r) const;

  // Reads the whole log (portal / diagnostics only).
  bool stats(HistoryStats* s);

  // Calls fn for every valid record with from <= epoch <= to, oldest first,
  // until fn returns false. Returns false on a read error.
  typedef bool (*Visitor)(void* ctx, const HistoryRecord& r);
  bool forEach(uint32_t from, uint32_t to, Visitor fn, void* ctx);

  // Record encoding (exposed for tests). out needs kHistoryMaxRecord bytes;
  // returns the record length. decode() applies one record to *state and
  // returns its length, 0 = not a record (blank or garbage), or sets *valid
  // to false on a CRC mismatch (skip len bytes).
  enum { kHistoryMaxRecord = 64, kHistoryKeySize = 34 };
  static size_t encodeKey(const HistoryRecord& r, uint32_t sectorSeq,
                          uint8_t* out);
  static size_t encodeDelta(const HistoryRecord& base, const HistoryRecord& r,
                            uint8_t* out);
  static size_t decode(const uint8_t* in, size_t avail, HistoryRecord* state,
                       uint32_t* sectorSeq, bool* valid);

 private:
  class Reader;
  size_t sectors() const;
  bool keyAt(size_t sector, HistoryRecord* r, uint32_t* seq);
  bool replay(size_t sector, Visitor fn, void* ctx, size_t* endOff,
              size_t* records, bool* stopped);
  int changed(const HistoryRecord& r) const;
  bool write(const HistoryRecord& r);

  HistoryStorage* st_ = nullptr;
  HistoryPolicy policy_;
  bool ok_ = false;
  bool hasBase_ = false;
  bool needKey_ = false;     // next record must start a new sector
  HistoryRecord base_ = {};  // last stored state
  size_t next_ = 0;          // byte offset of the next record
  uint32_t nextSectorSeq_ = 1;
};

// Binary export for the portal (GET /api/history/<from>/<to>), little endian:
//   header (16 B): "GMH3", uint16 rows, uint16 points (8),
//                  uint32 next (0 = complete, else request again from here),
//                  uint32 maxGap (s; rows further apart = no data between)
//   rows (24 B):   uint32 epoch, int16 centi[8] (INT16_MIN = missing),
//                  uint16 battery mV (0 = unknown), int8 rssi (0 = unknown),
//                  uint8 flags (HistoryFlags)
// Rows are the stored records (changes only): draw lines between them. The
// first row may be the last record before `from` (state at range start);
// the last row may be the held state at `holdUntil` (kHistHold).
enum { kHistoryHeaderSize = 16, kHistoryRowSize = 24 };

struct HistoryExportRequest {
  uint32_t from;
  uint32_t to;
  size_t maxRows;
  uint32_t maxGapS;    // heartbeat + 2 measurement intervals
  uint32_t holdUntil;  // time of the last measurement (0 = none)
};

size_t historyExport(HistoryLog& log, const HistoryExportRequest& req,
                     uint8_t* out, size_t cap);

}  // namespace gm

#endif
