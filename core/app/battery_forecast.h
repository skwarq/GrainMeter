// SPDX-License-Identifier: MIT
#ifndef GM_BATTERY_FORECAST_H
#define GM_BATTERY_FORECAST_H
#include <stddef.h>
#include <stdint.h>
namespace gm {
// Light-load LiPo SoC, rounded to integer percent; -1 = unknown voltage.
int batteryPercent(int mv);
// Runtime in hundredths of a day; -1 = unknown consumption/voltage.
int32_t batteryRuntimeCentiDays(int mv, uint32_t capacityMah,
                                int32_t mahPerDayCenti);
struct BatterySample {
  uint32_t epoch;
  uint32_t batteryMv;
};
struct BatteryForecast {
  enum State { Unknown, Discharging, Charging, Stable };
  State state = Unknown;
  int32_t days = -1;
  int32_t slopeMvPerDay = 0;
  uint32_t samples = 0;
};
// Input must be ordered oldest first; invalid and non-increasing points are
// ignored.
BatteryForecast batteryForecast(const BatterySample* pts, size_t n,
                                uint32_t nowEpoch, uint32_t critMv);
}  // namespace gm
#endif
