// Rough state of charge of a single Li-ion cell from its open-circuit voltage.
// This is an estimate only (load, temperature and cell ageing are ignored).

export const LIION_CURVE = [
  [3300, 0],
  [3500, 5],
  [3600, 10],
  [3700, 25],
  [3750, 35],
  [3800, 45],
  [3850, 55],
  [3900, 65],
  [4000, 80],
  [4100, 92],
  [4200, 100],
];

/** Percentage 0..100 (integer) or null when the voltage is unknown. */
export function batteryPercent(mv, curve = LIION_CURVE) {
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
