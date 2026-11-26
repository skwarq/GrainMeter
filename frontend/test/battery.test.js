import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  batteryPercent,
  batteryLevel,
  batteryForecast,
  batteryRuntimeDays,
  batteryRuntimeText,
  LIPO_CURVE,
} from '../src/lib/battery.js';
import { readFileSync } from 'node:fs';
import { translate } from '../src/i18n/translate.js';

test('C++ and JS use identical SoC knots and integer interpolation', () => {
  const cpp = readFileSync(new URL('../../core/app/battery_forecast.cpp', import.meta.url), 'utf8');
  const table = /kLipoCurve\[\]\[2\]\s*=\s*\{([\s\S]*?)\};/.exec(cpp);
  assert.ok(table);
  const knots = [...table[1].matchAll(/\{(\d+),\s*(\d+)\}/g)].map((m) => [Number(m[1]), Number(m[2])]);
  assert.deepEqual(knots, LIPO_CURVE);
  for (let mv = 3000; mv <= 4200; mv++) {
    const i = knots.findIndex(([v]) => mv <= v);
    if (i <= 0) continue;
    const [v0, p0] = knots[i - 1],
      [v1, p1] = knots[i];
    const pct = p0 + Math.floor(((mv - v0) * (p1 - p0) * 2 + v1 - v0) / (2 * (v1 - v0)));
    assert.equal(batteryPercent(mv), pct, `mV=${mv}`);
  }
});

test('runtime calculation: capacity, empty battery and missing data', () => {
  assert.equal(batteryRuntimeDays(3.5, 3840), 1250 / 3.5);
  assert.equal(batteryRuntimeDays(3.5, 3840, 5000), 2500 / 3.5);
  assert.equal(batteryRuntimeDays(3.5, 3000), 0);
  for (const mah of [null, undefined, 0, -1, NaN, Infinity]) assert.equal(batteryRuntimeDays(mah, 3840), null);
  assert.equal(batteryRuntimeDays(3.5, null), null);
  assert.equal(batteryRuntimeDays(3.5, 3840, 99), null);
  assert.equal(batteryRuntimeDays(3.5, 3840, 20001), null);
  assert.equal(batteryRuntimeDays(3.5, 3840, 2500.5), null);
  const t = (key, params) => translate('pl', key, params);
  assert.equal(
    batteryRuntimeText({ mah_per_day: 3.5, days: 357.14 }, t),
    'Czas pracy: ~357 dni (zużycie 3.50 mAh/dobę)',
  );
  assert.equal(batteryRuntimeText({ mah_per_day: 3.5, days: null }, t), 'Czas pracy: zbieranie danych');
});

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
