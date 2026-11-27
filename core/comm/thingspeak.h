// SPDX-License-Identifier: MIT
#ifndef GM_THINGSPEAK_H
#define GM_THINGSPEAK_H

#include <stddef.h>
#include <stdint.h>

#include "data/retained_state.h"

namespace gm {

// ThingSpeak bulk write (JSON):
//   POST <base>/channels/<id>/bulk_update.json, Content-Type application/json
//   {"write_api_key":"...","updates":[{"created_at":"...","field1":..,"status":".."}]}
// Success: HTTP 200 or 202 (the live API answers 202 Accepted) with
// {"success":true}. created_at must be unique within
// the channel; entries whose timestamp already exists are rejected by the
// server, which makes re-sending the same samples after a lost response
// idempotent. Max 960 entries per request (free license); >= 15 s between
// bulk calls on a channel.
enum { kTsMaxUpdatesPerRequest = 960 };

// "YYYY-MM-DD HH:MM:SS +0000" (format shown in the ThingSpeak bulk docs).
// 25 characters for years 0000-9999; buffers use kTsTimeBufLen.
enum { kTsTimeBufLen = 40 };
void tsFormatTime(uint32_t epoch, char* out, size_t cap);

// Wall clock of a sample: stored epoch, or reconstructed from the monotonic
// RTC time. Returns 0 if it cannot be determined (RTC restarted).
uint32_t tsSampleEpoch(const Sample& s, uint32_t nowEpoch, uint32_t nowMonoS);

struct BulkInfo {
  size_t entries;  // samples included
  uint32_t firstSeq;
  uint32_t lastSeq;  // remove up to this seq once confirmed
};

// Builds the bulk JSON from the oldest buffered samples (at most maxEntries).
// Samples whose time cannot be determined must have been removed already.
// Missing temperatures are omitted (never sent as 0). Returns false if the
// output buffer is too small even for one entry.
bool tsBuildBulk(const RetainedState& st, size_t maxEntries,
                 const char* writeKey, uint32_t nowEpoch, uint32_t nowMonoS,
                 char* out, size_t cap, BulkInfo* info);

struct DiagValues {
  int batteryMv;  // -1 = n/a (field omitted)
  int rssi;       // 0 = n/a
  uint32_t sensorsFound;
  uint32_t sensorErrors;
  uint32_t buffered;
  uint32_t dropped;
  uint32_t uploadFailures;
  uint32_t wakeCount;
  const char* status;  // short text (fw version, wake reason)
};

// Diagnostic status, max 255 bytes. Unknown mah/rt are omitted.
// uploadS/measureS are configured intervals, without power-mode factors.
// Returns false rather than publishing a truncated key/value.
bool tsFormatDiagStatus(const char* firmware, const char* wake,
                        const char* power, uint32_t capacityMah,
                        int32_t mahPerDayCenti, int32_t runtimeCentiDays,
                        uint32_t uploadS, uint32_t measureS, bool lanWake,
                        bool talkback, char* out, size_t cap);

bool tsBuildDiag(const DiagValues& d, const char* writeKey, uint32_t nowEpoch,
                 char* out, size_t cap);

// Diagnostic channel in bulk: one entry per buffered battery point
// (created_at = measurement time, field1 = mV) plus, when `full` is set, the
// complete diagnostic row at nowEpoch. Points without a reconstructable time
// are skipped. *points = battery entries written (the oldest ones).
bool tsBuildDiagBulk(const RetainedState& st, const DiagValues* full,
                     const char* writeKey, uint32_t nowEpoch, uint32_t nowMonoS,
                     char* out, size_t cap, size_t* points);

// URL "<base>/channels/<id>/bulk_update.json"
bool tsBulkUrl(const char* base, uint32_t channelId, char* out, size_t cap);

enum class TsOutcome { Confirmed, RetryLater, Rejected };
TsOutcome tsParseResponse(bool transportOk, int status, const char* body);
const char* tsOutcomeName(TsOutcome o);

}  // namespace gm

#endif
