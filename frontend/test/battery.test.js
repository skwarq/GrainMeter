import { test } from 'node:test';
import assert from 'node:assert/strict';
import { batteryPercent, batteryLevel } from '../src/lib/battery.js';

test('batteryPercent: bounds and interpolation', () => {
  assert.equal(batteryPercent(4200), 100);
  assert.equal(batteryPercent(4350), 100);
  assert.equal(batteryPercent(3300), 0);
  assert.equal(batteryPercent(2900), 0);
  assert.equal(batteryPercent(3950), 73); // 65 + 0.5 * 15 = 72.5 -> 73
  assert.equal(batteryPercent(3993), 79);
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
