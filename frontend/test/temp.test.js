import { test } from 'node:test';
import assert from 'node:assert/strict';
import { tempColor, formatTemp, isTemp, pointState, tempStats, TEMP_STOPS } from '../src/lib/temp.js';

test('tempColor: exact stops and clamping', () => {
  assert.equal(tempColor(-10), '#3e54c4');
  assert.equal(tempColor(-40), '#3e54c4');
  assert.equal(tempColor(40), '#d42c2c');
  assert.equal(tempColor(99), '#d42c2c');
  for (const s of TEMP_STOPS) assert.match(tempColor(s.t), /^#[0-9a-f]{6}$/);
});

test('tempColor: interpolates between stops', () => {
  // halfway between 0 (52,132,222) and 10 (38,178,172) -> (45,155,197)
  assert.equal(tempColor(5), '#2d9bc5');
});

test('tempColor: 0 °C is a real reading, null/undefined/NaN are not', () => {
  assert.equal(tempColor(0), '#3484de');
  assert.equal(tempColor(null), null);
  assert.equal(tempColor(undefined), null);
  assert.equal(tempColor(NaN), null);
  assert.equal(tempColor('12'), null);
});

test('formatTemp never shows a missing value as 0', () => {
  assert.equal(formatTemp(null), '—');
  assert.equal(formatTemp(undefined), '—');
  assert.equal(formatTemp(0), '0.0 °C');
  assert.equal(formatTemp(-0.5, 2), '-0.50 °C');
  assert.equal(isTemp(0), true);
  assert.equal(isTemp(null), false);
});

test('pointState: status handling', () => {
  assert.deepEqual(pointState({ point: 1, rom: 'A', status: 'ok', temp_c: 0 }), { valid: true, state: 'ok', temp: 0 });
  // ok status but null temperature is not a reading
  assert.equal(pointState({ point: 1, rom: 'A', status: 'ok', temp_c: null }).valid, false);
  // error statuses never expose a temperature (85 °C power-on value!)
  const e = pointState({ point: 2, rom: 'A', status: 'power_on_value', temp_c: 85 });
  assert.deepEqual(e, { valid: false, state: 'error', temp: null });
  assert.equal(pointState({ point: 3, rom: 'A', status: 'crc_error', temp_c: null }).state, 'error');
  assert.equal(pointState({ point: 4, rom: null, status: 'not_mapped', temp_c: null }).state, 'unmapped');
  assert.equal(pointState(undefined).state, 'unmapped');
});

test('tempStats ignores invalid points', () => {
  const s = tempStats([
    { point: 1, rom: 'A', status: 'ok', temp_c: 10 },
    { point: 2, rom: 'B', status: 'ok', temp_c: 0 },
    { point: 3, rom: 'C', status: 'missing', temp_c: null },
    { point: 4, rom: null, status: 'not_mapped', temp_c: null },
  ]);
  assert.deepEqual(s, { min: 0, max: 10, mean: 5, n: 2 });
  assert.equal(tempStats([{ point: 1, rom: 'A', status: 'missing', temp_c: null }]), null);
  assert.equal(tempStats(null), null);
});
