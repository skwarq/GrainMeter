import { batteryPercent } from '../lib/battery.js';
import { FIELD_COUNT } from '../lib/feed.js';

export const TEMPERATURE_CHANGE_C = 0.5;
export const BATTERY_STEP_PERCENT = 10;
export const DATA_GAP_MS = 2.5 * 3600 * 1000;
export const CHANGE_LIMIT = 200;

/** Changes within the loaded range; the first known value is a baseline. */
export function collectChanges(main, diag) {
  const out = [];
  const add = (t, type, kind, before, after, extra = {}) => out.push({ t, type, kind, before, after, ...extra });
  const points = Array.from({ length: FIELD_COUNT }, () => ({ value: null, status: null }));
  let alarm = null;
  for (const e of main?.entries || []) {
    for (let i = 0; i < FIELD_COUNT; i++) {
      const p = points[i];
      const v = e.values[i];
      const status = v !== null ? 'ok' : e.errors[i + 1] || 'missing';
      if (status !== p.status && (p.status !== null || (status !== 'ok' && status !== 'missing'))) {
        add(e.t, 'temperature', 'sensor', p.status, status, { point: i + 1 });
      }
      if (v !== null) {
        if (p.value !== null && Math.abs(v - p.value) >= TEMPERATURE_CHANGE_C - 1e-9) {
          add(e.t, 'temperature', 'temperature', p.value, v, { point: i + 1 });
        }
        p.value = v;
      }
      p.status = status;
    }
    const nextAlarm = e.kv.alarm || null;
    if (nextAlarm !== alarm) add(e.t, 'temperature', 'alarm', alarm, nextAlarm);
    alarm = nextAlarm;
  }

  let percent = null;
  let power = null;
  let firmware = null;
  let wakeCount = null;
  for (const e of diag?.entries || []) {
    const nextPercent = batteryPercent(e.values[0]);
    if (nextPercent !== null) {
      if (
        percent !== null &&
        Math.floor(nextPercent / BATTERY_STEP_PERCENT) !== Math.floor(percent / BATTERY_STEP_PERCENT)
      ) {
        add(e.t, 'battery', 'battery', percent, nextPercent);
      }
      percent = nextPercent;
    }
    // Battery-only rows have no status: absence is not a mode/version change.
    if (e.kv.power) {
      if (power !== null && power !== e.kv.power) add(e.t, 'battery', 'power', power, e.kv.power);
      power = e.kv.power;
    }
    if (e.kv.fw) {
      if (firmware !== null && firmware !== e.kv.fw) add(e.t, 'device', 'firmware', firmware, e.kv.fw);
      firmware = e.kv.fw;
    }
    const nextWakeCount = e.values[7];
    if (Number.isInteger(nextWakeCount) && nextWakeCount >= 0) {
      // field8 is retained across sleep. A decrease proves a counter reset,
      // not its cause; ordinary timer wakes and field5 buffer drains are not restarts.
      if (wakeCount !== null && nextWakeCount < wakeCount) {
        add(e.t, 'device', 'counterReset', wakeCount, nextWakeCount);
      }
      wakeCount = nextWakeCount;
    }
  }

  // Any measurement (including a battery-only row) proves the device reported.
  const times = [...(main?.entries || []), ...(diag?.entries || [])].map((e) => e.t).sort((a, b) => a - b);
  for (let i = 1; i < times.length; i++) {
    if (times[i] - times[i - 1] > DATA_GAP_MS) {
      add(times[i], 'device', 'gap', times[i - 1], times[i]);
    }
  }
  return out.sort((a, b) => b.t - a.t);
}

/** Apply the type filter before the row limit; total counts matching changes. */
export function visibleChanges(changes, type = 'all') {
  const matching = type === 'all' ? changes : changes.filter((c) => c.type === type);
  return { rows: matching.slice(0, CHANGE_LIMIT), total: matching.length };
}
