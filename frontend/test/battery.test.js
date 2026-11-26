import { test } from 'node:test';
import assert from 'node:assert/strict';
import { batteryPercent, batteryLevel, batteryForecast } from '../src/lib/battery.js';

test('batteryPercent: bounds and interpolation', () => {
  assert.equal(batteryPercent(4200), 100);
  assert.equal(batteryPercent(4350), 100);
  assert.equal(batteryPercent(3000), 0); // LP475085 cut-off
  assert.equal(batteryPercent(2900), 0);
  assert.equal(batteryPercent(3840), 50);
  assert.equal(batteryPercent(3650), 8); // default batt_low_mv
  assert.equal(batteryPercent(3500), 4); // default batt_crit_mv
  assert.equal(batteryPercent(3930), 68); // 65 + 0.5 * 5 = 67.5 -> 68
});

test('batteryPercent: unknown voltage is null, not 0 %', () => {
  assert.equal(batteryPercent(null), null);
  assert.equal(batteryPercent(undefined), null);
  assert.equal(batteryPercent(0), null);
  assert.equal(batteryPercent(NaN), null);
});

test('batteryPercent is monotonic', () => {
  let last = -1;
  for (let mv = 3200; mv <= 4300; mv += 10) {
    const p = batteryPercent(mv);
    assert.ok(p >= last);
    last = p;
  }
});

test('batteryLevel', () => {
  assert.equal(batteryLevel(null), 'unknown');
  assert.equal(batteryLevel(5), 'critical');
  assert.equal(batteryLevel(20), 'low');
  assert.equal(batteryLevel(80), 'ok');
});

const base = 1700000000000;
function series(hours, drop = 1) {
  return Array.from({ length: hours + 1 }, (_, h) => ({ t: base + h * 3600000, batteryMv: 3900 - h * drop }));
}
function forecast(p, crit = 3300) {
  return batteryForecast(p, p.at(-1).t, crit);
}
test('forecast: linear discharge and limits', () => {
  const p = series(48);
  assert.deepEqual(forecast(p), { state: 'discharging', days: 23, slope_mv_per_day: -24, samples: 49 });
  assert.equal(forecast(p, 4000).days, 0);
  assert.equal(forecast(p, 1).days, 160);
  const slow = series(48, 0).map((v, i) => ({ ...v, batteryMv: 3900 - Math.floor(i / 4) }));
  assert.equal(forecast(slow, 1).days, 365);
  const rising = series(24, 0).map((v, i) => ({ ...v, batteryMv: 3000 + i * 25 }));
  assert.equal(forecast(rising).state, 'charging');
});
test('forecast: solar charging offsets excluded', () => {
  const p = series(72);
  let mv = 3000;
  p.forEach((v, i) => {
    if (i) mv += i % 24 < 12 ? -2 : 30;
    v.batteryMv = mv;
  });
  assert.equal(forecast(p).state, 'discharging');
  assert.equal(forecast(p).slope_mv_per_day, -48);
});
test('forecast: gaps, insufficient data, stable, charging and stale data', () => {
  assert.equal(forecast(series(48, 0)).state, 'stable');
  assert.equal(forecast(series(12)).state, 'unknown');
  const sparse = [0, 24, 48, 72].map((h) => ({ t: base + h * 3600000, batteryMv: 3900 - h }));
  assert.equal(forecast(sparse).state, 'unknown');
  const gaps = series(72);
  gaps.splice(10, 14);
  assert.equal(forecast(gaps).slope_mv_per_day, -24);
  const full = series(24).map((v) => ({ ...v, batteryMv: 4200 }));
  assert.equal(forecast(full).state, 'charging');
  const p = series(48);
  assert.equal(batteryForecast(p, p.at(-1).t + 86400000).state, 'unknown');
  assert.equal(batteryForecast([], base).state, 'unknown');
});
