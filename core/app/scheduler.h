// SPDX-License-Identifier: MIT
#ifndef GM_SCHEDULER_H
#define GM_SCHEDULER_H

#include <stdint.h>

#include "app/config.h"
#include "data/retained_state.h"

namespace gm {

// Pure scheduling rules shared by the ESP32 wake cycle and the PC loop.
//  - measure every measureIntervalS (first wake measures immediately)
//  - upload once samplesPerUpload samples are buffered, but never more
//    often than uploadIntervalS since the previous attempt (so a dead
//    network costs one Wi-Fi window per upload interval, not per sample)
//  - sleep until the next measurement
struct Plan {
  bool measureDue;
  bool uploadDue;
  uint32_t sleepS;  // time until next measurement, >= 1
};

// Wakes a little early are tolerated (RTC drift): a deadline within this
// many seconds counts as reached.
enum { kScheduleToleranceS = 2 };

// Battery protection (thresholds from the configuration, 0 = disabled):
//  Normal   - schedule as configured
//  Economy  - battery < batt_low_mv: uploads 4x less often (samples are
//             still measured and buffered)
//  Critical - battery < batt_crit_mv: no radio at all, measuring interval
//             doubled; data waits in the buffer until the sun recharges
enum class PowerMode { Normal, Economy, Critical };
enum { kEconomyUploadFactor = 4, kCriticalMeasureFactor = 2 };

PowerMode powerModeFor(int batteryMv, const Config& cfg);
const char* powerModeName(PowerMode m);

Plan schedulePlan(const RetainedState& st, const Config& cfg, uint32_t nowMonoS,
                  PowerMode mode = PowerMode::Normal);

}  // namespace gm

#endif
