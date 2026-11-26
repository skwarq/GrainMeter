// SPDX-License-Identifier: MIT
#include "app/battery_forecast.h"

#include <math.h>
namespace gm {
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
