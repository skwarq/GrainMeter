// SPDX-License-Identifier: MIT
#include "app/scheduler.h"

namespace gm {

static bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  if (now < since) {
    return true;  // monotonic counter restarted: treat as overdue
  }
  return now - since + kScheduleToleranceS >= interval;
}

PowerMode powerModeFor(int batteryMv, const Config& cfg) {
  if (batteryMv < 0) {
    return PowerMode::Normal;  // no measurement available
  }
  const uint32_t mv = static_cast<uint32_t>(batteryMv);
  if (cfg.battCritMv != 0 && mv < cfg.battCritMv) {
    return PowerMode::Critical;
  }
  if (cfg.battLowMv != 0 && mv < cfg.battLowMv) {
    return PowerMode::Economy;
  }
  return PowerMode::Normal;
}

const char* powerModeName(PowerMode m) {
  switch (m) {
    case PowerMode::Normal:
      return "normal";
    case PowerMode::Economy:
      return "economy";
    case PowerMode::Critical:
      return "critical";
  }
  return "?";
}

Plan schedulePlan(const RetainedState& st, const Config& cfg, uint32_t nowMonoS,
                  PowerMode mode) {
  Plan p;
  const uint32_t measureS = mode == PowerMode::Critical
                                ? cfg.measureIntervalS * kCriticalMeasureFactor
                                : cfg.measureIntervalS;
  const uint32_t uploadS = mode == PowerMode::Economy
                               ? cfg.uploadIntervalS * kEconomyUploadFactor
                               : cfg.uploadIntervalS;
  p.measureDue =
      !st.hasMeasured || elapsed(nowMonoS, st.lastMeasureMonoS, measureS);

  const uint32_t needed = cfg.samplesPerUpload();
  const bool enough = st.count >= needed;
  const bool spaced = !st.hasUploadAttempt ||
                      elapsed(nowMonoS, st.lastUploadAttemptMonoS, uploadS);
  p.uploadDue = mode != PowerMode::Critical && cfg.uploadConfigured() &&
                st.count > 0 && enough && spaced;

  if (p.measureDue || !st.hasMeasured || nowMonoS < st.lastMeasureMonoS) {
    p.sleepS = measureS;  // after measuring now
  } else {
    const uint32_t since = nowMonoS - st.lastMeasureMonoS;
    p.sleepS = since >= measureS ? 1 : measureS - since;
  }
  if (p.sleepS == 0) {
    p.sleepS = 1;
  }
  return p;
}

}  // namespace gm
