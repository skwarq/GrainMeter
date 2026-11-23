import { test } from 'node:test';
import assert from 'node:assert/strict';
import { markerLayout, spreadLabels, SILO } from '../src/lib/silo.js';

const geom = { tipY: 300, topY: 100, count: 8 };
const pts = (heights) => heights.map((h, i) => ({ point: i + 1, rom: 'R' + i, height_cm: h }));

test('evenly spaced when no height is known, point 1 lowest', () => {
  const l = markerLayout(pts([null, null, null, null, null, null, null, null]), geom);
  assert.equal(l.length, 8);
  assert.equal(l[0].y, 300 - (0.5 / 8) * 200);
  assert.equal(l[7].y, 300 - (7.5 / 8) * 200);
  for (let i = 1; i < 8; i++) assert.ok(l[i].y < l[i - 1].y, 'higher point number is higher up');
  assert.ok(l.every((m) => !m.heightKnown));
});

test('-1 (unknown height from rom_map) counts as unknown', () => {
  const l = markerLayout([{ point: 1, rom: 'A', height_cm: -1 }], geom);
  assert.equal(l[0].heightKnown, false);
});

test('positions from height_cm: 0 = probe tip, to scale', () => {
  const l = markerLayout(pts([0, 100, 200]), geom);
  assert.equal(l[0].y, 300); // tip
  const scale = 200 * 1.08;
  assert.ok(Math.abs(l[1].y - (300 - (100 / scale) * 200)) < 1e-9);
  assert.ok(Math.abs(l[2].y - (300 - (200 / scale) * 200)) < 1e-9);
  assert.ok(l[2].y > geom.topY, 'highest stays inside the range');
  assert.ok(l.every((m) => m.heightKnown));
});

test('height order wins over point numbers', () => {
  const l = markerLayout(pts([300, 10]), geom);
  assert.ok(l[0].y < l[1].y, 'point 1 at 300 cm is above point 2 at 10 cm');
});

test('all heights 0 -> all at the tip', () => {
  assert.deepEqual(
    markerLayout(pts([0, 0]), geom).map((m) => m.y),
    [300, 300],
  );
});

test('mixed known/unknown heights fall back to even spacing', () => {
  const l = markerLayout(pts([null, 50, 20]), geom);
  assert.deepEqual(
    l.map((m) => m.y),
    [1, 2, 3].map((p) => 300 - ((p - 0.5) / 8) * 200),
  );
  assert.deepEqual(
    l.map((m) => m.heightKnown),
    [false, true, true],
  );
});

test('height mode leaves out points without a sensor', () => {
  const l = markerLayout([...pts([0, 100]), { point: 5, rom: null, height_cm: null }], geom);
  assert.deepEqual(
    l.map((m) => m.point),
    [1, 2],
  );
});

test('sorted by point and tolerant to bad input', () => {
  const l = markerLayout([{ point: 3, height_cm: null }, { point: 1, height_cm: null }, null], geom);
  assert.deepEqual(
    l.map((m) => m.point),
    [1, 3],
  );
  assert.deepEqual(markerLayout(null, geom), []);
  assert.ok(markerLayout(pts([null]), SILO)[0].y < SILO.tipY);
});

test('spreadLabels keeps a minimum gap and the bounds', () => {
  const ys = [200, 201, 202, 100];
  const out = spreadLabels(ys, 10, 50, 400);
  const sorted = [...out].sort((a, b) => a - b);
  for (let i = 1; i < sorted.length; i++) assert.ok(sorted[i] - sorted[i - 1] >= 10 - 1e-9);
  assert.equal(out[3], 100); // isolated label untouched
  const tight = spreadLabels([395, 396, 397], 10, 0, 400);
  assert.ok(Math.max(...tight) <= 400);
});
