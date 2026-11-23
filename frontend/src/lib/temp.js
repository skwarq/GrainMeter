// Temperature helpers. A missing reading is always `null` and is never shown
// or coloured as 0 °C (0.00 is a real reading).

/** Colour stops of the grain temperature scale (°C -> RGB). */
export const TEMP_STOPS = [
  { t: -10, rgb: [62, 84, 196] },
  { t: 0, rgb: [52, 132, 222] },
  { t: 10, rgb: [38, 178, 172] },
  { t: 18, rgb: [104, 186, 72] },
  { t: 25, rgb: [236, 190, 40] },
  { t: 32, rgb: [240, 124, 36] },
  { t: 40, rgb: [212, 44, 44] },
];

export const TEMP_MIN = TEMP_STOPS[0].t;
export const TEMP_MAX = TEMP_STOPS[TEMP_STOPS.length - 1].t;

/** Neutral colour for points without a valid reading. */
export const NO_READING_COLOR = '#8a8f98';

/** True for a finite number (a real reading, including 0). */
export function isTemp(v) {
  return typeof v === 'number' && Number.isFinite(v);
}

function hex(n) {
  return Math.round(n).toString(16).padStart(2, '0');
}

/** Colour for a temperature; `null` for anything that is not a reading. */
export function tempColor(t) {
  if (!isTemp(t)) return null;
  if (t <= TEMP_MIN) return '#' + TEMP_STOPS[0].rgb.map(hex).join('');
  if (t >= TEMP_MAX) return '#' + TEMP_STOPS[TEMP_STOPS.length - 1].rgb.map(hex).join('');
  for (let i = 1; i < TEMP_STOPS.length; i++) {
    const a = TEMP_STOPS[i - 1];
    const b = TEMP_STOPS[i];
    if (t <= b.t) {
      const f = (t - a.t) / (b.t - a.t);
      return '#' + a.rgb.map((c, k) => hex(c + (b.rgb[k] - c) * f)).join('');
    }
  }
  return null;
}

/** "12.3 °C" or the placeholder for a missing value. */
export function formatTemp(t, digits = 1, missing = '—') {
  if (!isTemp(t)) return missing;
  return t.toFixed(digits) + ' °C';
}

/**
 * Normalised view of a measuring point:
 *  - valid: status "ok" and a numeric temperature
 *  - state: "ok" | "error" | "unmapped"
 */
export function pointState(p) {
  if (!p) return { valid: false, state: 'unmapped', temp: null };
  if (p.status === 'not_mapped' || !p.rom) return { valid: false, state: 'unmapped', temp: null };
  if (p.status === 'ok' && isTemp(p.temp_c)) return { valid: true, state: 'ok', temp: p.temp_c };
  return { valid: false, state: 'error', temp: null };
}

/** Min / max / mean of valid readings, or null when there is none. */
export function tempStats(points) {
  const vals = (points || [])
    .map(pointState)
    .filter((s) => s.valid)
    .map((s) => s.temp);
  if (vals.length === 0) return null;
  const sum = vals.reduce((a, b) => a + b, 0);
  return { min: Math.min(...vals), max: Math.max(...vals), mean: sum / vals.length, n: vals.length };
}
