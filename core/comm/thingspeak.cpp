// SPDX-License-Identifier: MIT
#include "comm/thingspeak.h"

#include <stdio.h>
#include <string.h>

#include "util/json.h"

namespace gm {

bool tsFormatDiagStatus(const char* firmware, const char* wake,
                        const char* power, uint32_t capacityMah,
                        int32_t mahPerDayCenti, int32_t runtimeCentiDays,
                        char* out, size_t cap) {
  if (cap > 256) cap = 256;
  int n = snprintf(out, cap, "fw=%s wake=%s power=%s capacity=%lu", firmware,
                   wake, power, static_cast<unsigned long>(capacityMah));
  if (n < 0 || static_cast<size_t>(n) >= cap) return false;
  const int32_t values[] = {mahPerDayCenti, runtimeCentiDays};
  const char* keys[] = {"mah", "rt"};
  for (size_t i = 0; i < 2; ++i) {
    if (values[i] < 0) continue;
    const int added = snprintf(
        out + n, cap - static_cast<size_t>(n), " %s=%ld.%02ld", keys[i],
        static_cast<long>(values[i] / 100), static_cast<long>(values[i] % 100));
    if (added < 0 || static_cast<size_t>(added) >= cap - static_cast<size_t>(n))
      return false;
    n += added;
  }
  return true;
}

void tsFormatTime(uint32_t epoch, char* out, size_t cap) {
  // Civil-from-days (H. Hinnant), independent of TZ and gmtime_r.
  const uint32_t secs = epoch % 86400u;
  int64_t z = static_cast<int64_t>(epoch / 86400u) + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) {
    ++y;
  }
  snprintf(out, cap, "%04d-%02d-%02d %02u:%02u:%02u +0000", static_cast<int>(y),
           static_cast<int>(m), static_cast<int>(d),
           static_cast<unsigned>(secs / 3600u),
           static_cast<unsigned>((secs / 60u) % 60u),
           static_cast<unsigned>(secs % 60u));
}

uint32_t tsSampleEpoch(const Sample& s, uint32_t nowEpoch, uint32_t nowMonoS) {
  if (s.epoch != 0) {
    return s.epoch;
  }
  if (nowEpoch == 0 || nowMonoS < s.monoS) {
    return 0;  // RTC counter restarted after capture: time unknown
  }
  const uint32_t age = nowMonoS - s.monoS;
  return age < nowEpoch ? nowEpoch - age : 0;
}

bool tsBulkUrl(const char* base, uint32_t channelId, char* out, size_t cap) {
  const int n = snprintf(out, cap, "%s/channels/%lu/bulk_update.json", base,
                         static_cast<unsigned long>(channelId));
  return n > 0 && static_cast<size_t>(n) < cap;
}

static void writeStatusText(const Sample& s, char* out, size_t cap) {
  // "seq=123" plus compact error list "p3:crc_error,p5:missing" so gaps
  // are explained in the ThingSpeak channel feed itself.
  int n = snprintf(out, cap, "seq=%lu", static_cast<unsigned long>(s.seq));
  for (int p = 0; p < kMaxPoints && n > 0 && static_cast<size_t>(n) < cap;
       ++p) {
    const ReadStatus st = static_cast<ReadStatus>(s.status[p]);
    if (st == ReadStatus::Ok || st == ReadStatus::NotMapped) {
      continue;
    }
    n += snprintf(out + n, cap - static_cast<size_t>(n), " p%d:%s", p + 1,
                  readStatusName(st));
  }
}

bool tsBuildBulk(const RetainedState& st, size_t maxEntries,
                 const char* writeKey, uint32_t nowEpoch, uint32_t nowMonoS,
                 char* out, size_t cap, BulkInfo* info) {
  info->entries = 0;
  info->firstSeq = 0;
  info->lastSeq = 0;
  if (maxEntries > kTsMaxUpdatesPerRequest) {
    maxEntries = kTsMaxUpdatesPerRequest;
  }
  // If the JSON does not fit, retry with fewer entries; the rest is sent
  // in the next upload.
  size_t limit = st.count < maxEntries ? st.count : maxEntries;
  while (limit > 0) {
    JsonWriter w(out, cap);
    w.beginObject().kv("write_api_key", writeKey).key("updates").beginArray();
    uint32_t prevTs = 0;
    size_t n = 0;
    uint32_t lastSeq = 0;
    for (size_t i = 0; i < limit && w.ok(); ++i) {
      const Sample& s = st.at(i);
      uint32_t ts = tsSampleEpoch(s, nowEpoch, nowMonoS);
      if (ts == 0) {
        continue;  // caller should have purged these
      }
      if (ts <= prevTs) {
        ts = prevTs + 1;  // created_at must be unique within a request
      }
      prevTs = ts;
      char tbuf[kTsTimeBufLen];
      tsFormatTime(ts, tbuf, sizeof(tbuf));
      w.beginObject().kv("created_at", tbuf);
      for (int p = 0; p < kMaxPoints; ++p) {
        if (!sampleHasValue(s, p)) {
          continue;  // missing value is omitted, never sent as 0
        }
        char fname[8];
        snprintf(fname, sizeof(fname), "field%d", p + 1);
        w.key(fname).centi(s.centi[p]);
      }
      char status[160];
      writeStatusText(s, status, sizeof(status));
      if (st.alarm.active) {
        const size_t len = strlen(status);
        snprintf(status + len, sizeof(status) - len, " alarm=%s",
                 st.alarm.reason == AlarmTemp ? "temp" : "rise");
      }
      w.kv("status", status).endObject();
      if (w.ok()) {
        if (n == 0) {
          info->firstSeq = s.seq;
        }
        ++n;
        lastSeq = s.seq;
      }
    }
    w.endArray().endObject();
    if (w.ok()) {
      info->entries = n;
      info->lastSeq = lastSeq;
      return n > 0;
    }
    limit = (n < limit) ? n : limit - 1;
  }
  return false;
}

bool tsBuildDiag(const DiagValues& d, const char* writeKey, uint32_t nowEpoch,
                 char* out, size_t cap) {
  char tbuf[kTsTimeBufLen];
  tsFormatTime(nowEpoch, tbuf, sizeof(tbuf));
  JsonWriter w(out, cap);
  w.beginObject().kv("write_api_key", writeKey).key("updates").beginArray();
  w.beginObject().kv("created_at", tbuf);
  if (d.batteryMv >= 0) {
    w.kv("field1", static_cast<int64_t>(d.batteryMv));
  }
  if (d.rssi != 0) {
    w.kv("field2", static_cast<int64_t>(d.rssi));
  }
  w.kvu("field3", d.sensorsFound)
      .kvu("field4", d.sensorErrors)
      .kvu("field5", d.buffered)
      .kvu("field6", d.dropped)
      .kvu("field7", d.uploadFailures)
      .kvu("field8", d.wakeCount)
      .kv("status", d.status != nullptr ? d.status : "")
      .endObject();
  w.endArray().endObject();
  return w.ok();
}

static void writeDiagFields(JsonWriter& w, const DiagValues& d) {
  if (d.batteryMv >= 0) {
    w.kv("field1", static_cast<int64_t>(d.batteryMv));
  }
  if (d.rssi != 0) {
    w.kv("field2", static_cast<int64_t>(d.rssi));
  }
  w.kvu("field3", d.sensorsFound)
      .kvu("field4", d.sensorErrors)
      .kvu("field5", d.buffered)
      .kvu("field6", d.dropped)
      .kvu("field7", d.uploadFailures)
      .kvu("field8", d.wakeCount)
      .kv("status", d.status != nullptr ? d.status : "");
}

bool tsBuildDiagBulk(const RetainedState& st, const DiagValues* full,
                     const char* writeKey, uint32_t nowEpoch, uint32_t nowMonoS,
                     char* out, size_t cap, size_t* points) {
  *points = 0;
  JsonWriter w(out, cap);
  w.beginObject().kv("write_api_key", writeKey).key("updates").beginArray();
  char tbuf[kTsTimeBufLen];
  uint32_t prev = 0;
  for (size_t i = 0; i < st.battCount; ++i) {
    const BatteryPoint& p = st.battAt(i);
    uint32_t ts = p.epoch;
    if (ts == 0 && nowEpoch != 0 && nowMonoS >= p.monoS &&
        nowMonoS - p.monoS < nowEpoch) {
      ts = nowEpoch - (nowMonoS - p.monoS);
    }
    ++*points;  // consumed even when skipped (no time): never sendable
    // The full row carries the current voltage; created_at must be unique.
    if (ts == 0 || ts <= prev || (full != nullptr && ts + 1 >= nowEpoch)) {
      continue;
    }
    prev = ts;
    tsFormatTime(ts, tbuf, sizeof(tbuf));
    w.beginObject()
        .kv("created_at", tbuf)
        .kv("field1", static_cast<int64_t>(p.mv))
        .endObject();
  }
  if (full != nullptr) {
    tsFormatTime(nowEpoch, tbuf, sizeof(tbuf));
    w.beginObject().kv("created_at", tbuf);
    writeDiagFields(w, *full);
    w.endObject();
  }
  w.endArray().endObject();
  return w.ok();
}

static bool bodyHasSuccessTrue(const char* body) {
  const char* p = strstr(body, "\"success\"");
  if (p == nullptr) {
    return false;
  }
  p += 9;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  if (*p != ':') {
    return false;
  }
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return strncmp(p, "true", 4) == 0;
}

TsOutcome tsParseResponse(bool transportOk, int status, const char* body) {
  if (!transportOk) {
    return TsOutcome::RetryLater;
  }
  // The documentation shows 200; the live API answers bulk writes with
  // 202 Accepted. Either counts only together with {"success":true}.
  const bool accepted = status == 200 || status == 202;
  if (accepted && body != nullptr && bodyHasSuccessTrue(body)) {
    return TsOutcome::Confirmed;
  }
  if (status == 429 || status >= 500 || accepted) {
    return TsOutcome::RetryLater;  // 200 without success:true is not a
                                   // confirmation
  }
  return TsOutcome::Rejected;  // 400/401/402/404/413/422...: needs user action
}

const char* tsOutcomeName(TsOutcome o) {
  switch (o) {
    case TsOutcome::Confirmed:
      return "confirmed";
    case TsOutcome::RetryLater:
      return "retry_later";
    case TsOutcome::Rejected:
      return "rejected";
  }
  return "?";
}

}  // namespace gm
