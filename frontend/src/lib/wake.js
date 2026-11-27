// Radio windows are anchored to the newest reported measurement, not the
// browser's fetch time.
const MINUTE = 60 * 1000;
const MARGIN_MS = 2 * MINUTE;
const PREPARE_MS = 3 * MINUTE;

export function latestWindowMs(main, diag) {
  const times = [main, diag].map((feed) => feed?.entries.at(-1)?.t).filter(Number.isFinite);
  return times.length ? Math.max(...times) : null;
}

export function predictWake({ lastWindowMs, uploadS, measureS, power, nowMs, count = 3 }) {
  const result = {
    state: 'unknown',
    windows: [],
    intervalMs: null,
    marginMs: MARGIN_MS,
    prepareByMs: null,
    uncertain: false,
  };
  if (power === 'critical') return { ...result, state: 'critical' };
  const configured = Number.isInteger(uploadS) && uploadS > 0 && Number.isInteger(measureS) && measureS > 0;
  // Apply economy before rounding to a measurement wake (scheduler.cpp).
  const intervalMs = configured
    ? Math.ceil((uploadS * (power === 'economy' ? 4 : 1)) / measureS) * measureS * 1000
    : null;
  if (
    !Number.isFinite(lastWindowMs) ||
    !Number.isFinite(nowMs) ||
    lastWindowMs > nowMs + MARGIN_MS ||
    !Number.isFinite(intervalMs) ||
    intervalMs <= 0 ||
    !Number.isInteger(count) ||
    count < 1
  ) {
    return result;
  }
  const steps = Math.max(1, Math.floor((nowMs - lastWindowMs) / intervalMs) + 1);
  const windows = Array.from({ length: count }, (_, i) => lastWindowMs + (steps + i) * intervalMs);
  return {
    ...result,
    state: nowMs - lastWindowMs > intervalMs + MARGIN_MS ? 'overdue' : 'ok',
    windows,
    intervalMs,
    prepareByMs: windows[0] - PREPARE_MS,
    uncertain: steps > 1 || !['normal', 'economy'].includes(power),
  };
}
