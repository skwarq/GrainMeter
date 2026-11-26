// SPDX-License-Identifier: MIT
#include "app/battery_forecast.h"

#include <math.h>
namespace gm {
namespace {
const int kLipoCurve[][2] = {
    {3000, 0},  {3300, 2},  {3500, 4},  {3610, 6},  {3690, 10}, {3710, 15},
    {3730, 20}, {3750, 25}, {3770, 30}, {3790, 35}, {3800, 40}, {3820, 45},
    {3840, 50}, {3850, 55}, {3870, 60}, {3910, 65}, {3950, 70}, {3980, 75},
    {4020, 80}, {4080, 85}, {4110, 90}, {4150, 95}, {4200, 100}};
}

int batteryPercent(int mv) {
  if (mv <= 0) return -1;
  if (mv <= kLipoCurve[0][0]) return 0;
  for (size_t i = 1; i < sizeof(kLipoCurve) / sizeof(kLipoCurve[0]); ++i) {
    if (mv <= kLipoCurve[i][0]) {
      const int* lo = kLipoCurve[i - 1];
      const int* hi = kLipoCurve[i];
      const int span = hi[0] - lo[0];
      return lo[1] + ((mv - lo[0]) * (hi[1] - lo[1]) * 2 + span) / (2 * span);
    }
  }
  return 100;
}

int32_t batteryRuntimeCentiDays(int mv, uint32_t capacityMah,
                                int32_t mahPerDayCenti) {
  const int soc = batteryPercent(mv);
  if (soc < 0 || mahPerDayCenti <= 0 || capacityMah < 100 ||
      capacityMah > 20000)
    return -1;
  return static_cast<int32_t>(
      (static_cast<int64_t>(capacityMah) * soc * 100 + mahPerDayCenti / 2) /
      mahPerDayCenti);
}

BatteryForecast batteryForecast(const BatterySample* pts, size_t n,
                                uint32_t nowEpoch, uint32_t critMv) {
  BatteryForecast f;
  BatterySample prev = {};
  uint32_t first = 0, last = 0, voltage = 0;
  double duration = 0, delta = 0, recent = 0, charging = 0;
  if (!pts || !nowEpoch) return f;
  for (size_t i = 0; i < n; ++i) {
    const BatterySample& p = pts[i];
    if (!p.epoch || p.epoch > nowEpoch || nowEpoch - p.epoch > 604800 ||
        !p.batteryMv || (prev.epoch && p.epoch <= prev.epoch))
      continue;
    if (!first) first = p.epoch;
    last = p.epoch;
    voltage = p.batteryMv;
    ++f.samples;
    if (prev.epoch) {
      const double dt = p.epoch - prev.epoch;
      const double dv = static_cast<double>(p.batteryMv) - prev.batteryMv;
      // Do not infer a trend across missing nights/days. Regress each
      // two-point segment with its own intercept, weighting slopes by time:
      // charging offsets never contaminate subsequent discharge segments.
      if (dt <= 21600) {
        const bool charge =
            dv * 3600 / dt > 20 || p.batteryMv > 4150 || prev.batteryMv > 4150;
        const uint32_t cutoff = nowEpoch > 86400 ? nowEpoch - 86400 : 0;
        const double overlap =
            p.epoch > cutoff
                ? p.epoch - (prev.epoch > cutoff ? prev.epoch : cutoff)
                : 0;
        recent += overlap;
        if (charge)
          charging += overlap;
        else {
          duration += dt;
          delta += dv;
        }
      }
    }
    prev = p;
  }
  if (f.samples < 4 || last - first < 86400 || nowEpoch - last > 21600)
    return f;
  if (recent >= 43200 && charging > recent / 2) {
    f.state = BatteryForecast::Charging;
    return f;
  }
  if (duration < 86400) return f;
  const double slope = delta * 86400 / duration;
  f.slopeMvPerDay = static_cast<int32_t>(floor(slope + 0.5));
  if (fabs(slope) < 5)
    f.state = BatteryForecast::Stable;
  else if (slope < 0) {
    f.state = BatteryForecast::Discharging;
    // Project the newest observed voltage forward to now as well.
    const double days = (static_cast<double>(voltage) - critMv) / -slope -
                        (nowEpoch - last) / 86400.0;
    f.days = static_cast<int32_t>(floor(fmax(0, fmin(365, days)) + 0.5));
  }
  return f;
}
}  // namespace gm
