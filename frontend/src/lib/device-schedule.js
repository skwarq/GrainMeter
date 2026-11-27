// Device schedule remembered from the last connection to the board through
// the remote portal (remote.html). It shares the origin with the dashboard,
// so the dashboard can predict wake-ups also for firmware whose diagnostic
// status has no up=/ms=. The local portal (board's own address) is another
// origin and cannot reach this storage.
export const SCHEDULE_KEY = 'gm.device.schedule';

const posInt = (v) => (Number.isInteger(v) && v > 0 ? v : undefined);
const bool = (v) => (typeof v === 'boolean' ? v : undefined);

export function loadDeviceSchedule(storage = globalThis.localStorage) {
  try {
    const v = JSON.parse(storage.getItem(SCHEDULE_KEY));
    return v && typeof v === 'object' ? v : null;
  } catch {
    return null;
  }
}

/** Merges known fields from /api/status or /api/config; writes only on change. */
export function rememberDeviceSchedule(fields, storage = globalThis.localStorage, nowMs = Date.now()) {
  const clean = {
    upload_s: posInt(fields.upload_s),
    measure_s: posInt(fields.measure_s),
    lan_wake: bool(fields.lan_wake),
    talkback: bool(fields.talkback),
  };
  const known = Object.fromEntries(Object.entries(clean).filter(([, v]) => v !== undefined));
  if (!Object.keys(known).length) return;
  try {
    const old = loadDeviceSchedule(storage) || {};
    const next = { ...old, ...known };
    const same = Object.keys(known).every((k) => old[k] === known[k]);
    // Refresh the date at most hourly: it only tells how old the values are.
    if (same && nowMs - (old.savedAt || 0) < 3600000) return;
    storage.setItem(SCHEDULE_KEY, JSON.stringify({ ...next, savedAt: nowMs }));
  } catch {
    /* no storage */
  }
}

export function rememberFromStatus(s, storage) {
  const sch = (s && s.schedule) || {};
  rememberDeviceSchedule({ upload_s: sch.upload_s, measure_s: sch.measure_s }, storage);
}

export function rememberFromConfig(c, storage) {
  if (!c) return;
  rememberDeviceSchedule(
    {
      upload_s: c.upload_s,
      measure_s: c.measure_s,
      lan_wake: c.lan_wake,
      talkback: typeof c.talkback_id === 'number' ? c.talkback_id !== 0 : undefined,
    },
    storage,
  );
}
