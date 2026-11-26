// State of charge of the AKYGA LP475085 LiPo (LiCoO2, 4.2 V charge, 3.0 V
// cut-off, docs/hardware.md) from its voltage at light load: typical LiPo
// open-circuit curve, the datasheet gives no curve. An estimate only (load,
// temperature and cell ageing are ignored).
export const LIPO_CURVE = [
  [3000, 0],
  [3300, 2],
  [3500, 4],
  [3610, 6],
  [3690, 10],
  [3710, 15],
  [3730, 20],
  [3750, 25],
  [3770, 30],
  [3790, 35],
  [3800, 40],
  [3820, 45],
  [3840, 50],
  [3850, 55],
  [3870, 60],
  [3910, 65],
  [3950, 70],
  [3980, 75],
  [4020, 80],
  [4080, 85],
  [4110, 90],
  [4150, 95],
  [4200, 100],
];
export const LIION_CURVE = LIPO_CURVE; // previous name

/** Percentage 0..100 (integer) or null when the voltage is unknown. */
export function batteryPercent(mv, curve = LIPO_CURVE) {
  if (typeof mv !== 'number' || !Number.isFinite(mv) || mv <= 0) return null;
  if (mv <= curve[0][0]) return 0;
  const last = curve[curve.length - 1];
  if (mv >= last[0]) return 100;
  for (let i = 1; i < curve.length; i++) {
    const [v1, p1] = curve[i];
    if (mv <= v1) {
      const [v0, p0] = curve[i - 1];
      return Math.round(p0 + ((mv - v0) / (v1 - v0)) * (p1 - p0));
    }
  }
  return 100;
}

/** "ok" | "low" | "critical" | "unknown" */
export function batteryLevel(pct) {
  if (pct === null || pct === undefined) return 'unknown';
  if (pct <= 10) return 'critical';
  if (pct <= 25) return 'low';
  return 'ok';
}

/** Runtime in days from consumption and voltage; null without usable data. */
export function batteryRuntimeDays(mah, mv, capacity = 2500) {
  const soc = batteryPercent(mv);
  if (
    !Number.isFinite(mah) ||
    mah <= 0 ||
    soc === null ||
    !Number.isInteger(capacity) ||
    capacity < 100 ||
    capacity > 20000
  )
    return null;
  return (capacity * soc) / 100 / mah;
}

export function batteryRuntimeText(runtime, t) {
  if (
    !runtime ||
    !Number.isFinite(runtime.days) ||
    runtime.days < 0 ||
    !Number.isFinite(runtime.mah_per_day) ||
    runtime.mah_per_day <= 0
  )
    return t('batteryRuntime.collecting');
  return t('batteryRuntime.text', {
    days: Math.round(runtime.days),
    mah: runtime.mah_per_day.toFixed(2),
  });
}

/** Ordered {t: UTC milliseconds, batteryMv} points; same segment estimator as core. */
export function batteryForecast(points, nowMs, critMv = 3500) {
  const f = { state: 'unknown', days: null, slope_mv_per_day: 0, samples: 0 };
  const now = Math.floor(nowMs / 1000);
  let prev = null,
    first = 0,
    last = 0,
    voltage = 0;
  let duration = 0,
    delta = 0,
    recent = 0,
    charging = 0;
  if (!Number.isFinite(now) || now <= 0) return f;
  for (const point of points || []) {
    const p = { epoch: Math.floor(point.t / 1000), batteryMv: point.batteryMv };
    if (
      !Number.isFinite(p.epoch) ||
      p.epoch <= 0 ||
      p.epoch > now ||
      now - p.epoch > 604800 ||
      !Number.isFinite(p.batteryMv) ||
      p.batteryMv <= 0 ||
      (prev && p.epoch <= prev.epoch)
    )
      continue;
    if (!first) first = p.epoch;
    last = p.epoch;
    voltage = p.batteryMv;
    f.samples++;
    if (prev) {
      const dt = p.epoch - prev.epoch,
        dv = p.batteryMv - prev.batteryMv;
      // Independent two-point regressions weighted by duration exclude charge offsets.
      if (dt <= 21600) {
        const charge = (dv * 3600) / dt > 20 || p.batteryMv > 4150 || prev.batteryMv > 4150;
        const overlap = Math.max(0, p.epoch - Math.max(prev.epoch, now - 86400));
        recent += overlap;
        if (charge) charging += overlap;
        else {
          duration += dt;
          delta += dv;
        }
      }
    }
    prev = p;
  }
  if (f.samples < 4 || last - first < 86400 || now - last > 21600) return f;
  if (recent >= 43200 && charging > recent / 2) {
    f.state = 'charging';
    return f;
  }
  if (duration < 86400) return f;
  const slope = (delta * 86400) / duration;
  f.slope_mv_per_day = Math.round(slope) || 0;
  if (Math.abs(slope) < 5) f.state = 'stable';
  else if (slope < 0) {
    f.state = 'discharging';
    f.days = Math.round(Math.max(0, Math.min(365, (voltage - critMv) / -slope - (now - last) / 86400)));
  }
  return f;
}

export function batteryForecastText(forecast, t) {
  const f = forecast || { state: 'unknown' };
  return t('batteryForecast.' + f.state, { days: f.days });
}
