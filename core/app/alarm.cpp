// SPDX-License-Identifier: MIT
#include "app/alarm.h"

#include <string.h>
namespace gm {
const char* alarmReasonName(uint8_t reason) {
  return reason == AlarmTemp ? "temp" : reason == AlarmRise ? "rise" : "none";
}
void alarmEnqueue(AlarmState& a, const NotifyEvent& e) {
  if (a.count == kNotifySlots) {
    memmove(a.events, a.events + 1, (kNotifySlots - 1) * sizeof(NotifyEvent));
    --a.count;
    ++a.dropped;
  }
  a.events[a.count++] = e;
}
struct Baseline {
  HistoryRecord row;
  bool found;
};
static bool visitBaseline(void* ctx, const HistoryRecord& r) {
  Baseline& b = *static_cast<Baseline*>(ctx);
  b.row = r;
  b.found = true;
  return true;
}
void alarmEvaluate(AlarmState& a, const Config& cfg, const MeasureReport& m,
                   HistoryLog& history, uint32_t maxGapS) {
  Baseline b = {};
  const uint32_t span = cfg.alarmRiseH * 3600u;
  // Use the last known state at the target time, only within a bounded gap.
  if (cfg.alarmRiseCenti && m.epoch > span && history.available()) {
    const uint32_t target = m.epoch - span;
    if (!history.forEach(target > maxGapS ? target - maxGapS : 0, target,
                         visitBaseline, &b))
      b.found = false;
  }
  uint8_t reason = AlarmNone, point = 0;
  int16_t value = 0, rise = 0;
  uint8_t seen = 0;
  for (size_t i = 0; i < m.count; ++i) {
    const SensorReading& r = m.readings[i];
    if (!r.point || r.point > kMaxPoints) continue;
    const uint8_t bit = static_cast<uint8_t>(1u << (r.point - 1));
    seen |= bit;
    if (!cfg.alarmTempCenti) a.tempMask &= ~bit;
    if (r.status != ReadStatus::Ok) {
      // A failed reading cannot establish that a hot point has cooled.
      if ((a.tempMask & bit) && reason == AlarmNone) {
        reason = AlarmTemp;
        point = r.point;
        value = a.centi;
      }
      continue;
    }
    if (cfg.alarmTempCenti) {
      if (r.centi >= static_cast<int>(cfg.alarmTempCenti))
        a.tempMask |= bit;
      else if (r.centi <= static_cast<int>(cfg.alarmTempCenti) - 100)
        a.tempMask &= ~bit;
    }
    const int p = r.point - 1;
    const bool haveBase =
        b.found && b.row.centi[p] != kHistoryMissing &&
        b.row.status[p] == static_cast<uint8_t>(ReadStatus::Ok);
    const int delta = haveBase ? r.centi - b.row.centi[p] : 0;
    if (a.tempMask & bit) {
      reason = AlarmTemp;
      point = r.point;
      value = r.centi;
      rise = static_cast<int16_t>(delta);
    } else if (reason != AlarmTemp && cfg.alarmRiseCenti && haveBase &&
               delta >= static_cast<int>(cfg.alarmRiseCenti)) {
      reason = AlarmRise;
      point = r.point;
      value = r.centi;
      rise = static_cast<int16_t>(delta);
    }
  }
  a.tempMask &= seen;
  const bool wasActive = a.active != 0;
  if (reason != AlarmNone) {
    a.active = 1;
    a.reason = reason;
    a.point = point;
    a.centi = value;
    a.riseCenti = rise;
    if (!wasActive) {
      a.startEpoch = m.epoch;
      a.startMonoS = m.monoS;
      a.pendingWindow = 1;
      NotifyEvent e = {m.epoch,     m.monoS, value, rise,
                       NotifyAlarm, reason,  point, 0};
      alarmEnqueue(a, e);
    }
  } else if (wasActive) {
    a.active = 0;
    NotifyEvent e = {m.epoch,     m.monoS,  a.centi, a.riseCenti,
                     NotifyClear, a.reason, a.point, 0};
    alarmEnqueue(a, e);
    a.reason = AlarmNone;
  }
}
bool alarmWindowDue(const AlarmState& a, const Config& cfg, uint32_t now) {
  return a.pendingWindow || (a.active && cfg.alarmRepeatMin &&
                             (!a.hasWindow || now - a.lastWindowMonoS >=
                                                  cfg.alarmRepeatMin * 60u));
}
}  // namespace gm
