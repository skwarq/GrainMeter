import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  alerts,
  change24h,
  downsample,
  gapThreshold,
  heightFromLabel,
  latestDiag,
  latestPoints,
  niceTicks,
  parseFeed,
  parseStatus,
  pointStats,
  segments,
  thingSpeakTime,
  toNumber,
} from '../src/lib/feed.js';

const H = 3600 * 1000;
const T0 = Date.parse('2026-11-20T00:00:00Z');

function iso(ms) {
  return new Date(ms).toISOString().replace('.000', '');
}

// hourly entries; fn(hour) -> {field1..8, status}
function feed(hours, fn, channel = {}) {
  const feeds = [];
  for (let h = 0; h < hours; h++) feeds.push({ created_at: iso(T0 + h * H), entry_id: h + 1, ...fn(h) });
  return parseFeed({ channel: { name: 'Silos', ...channel }, feeds });
}

test('toNumber: missing values are null, 0 is a value', () => {
  assert.equal(toNumber(null), null);
  assert.equal(toNumber(undefined), null);
  assert.equal(toNumber(''), null);
  assert.equal(toNumber('abc'), null);
  assert.equal(toNumber('0.00'), 0);
  assert.equal(toNumber('-3.25'), -3.25);
});

test('parseStatus: sequence, sensor errors and key/values', () => {
  const s = parseStatus('seq=12 p3:crc_error p5:missing');
  assert.equal(s.seq, 12);
  assert.deepEqual(s.errors, { 3: 'crc_error', 5: 'missing' });
  assert.deepEqual(parseStatus('fw=0.1.1 wake=timer power=economy').kv, {
    fw: '0.1.1',
    wake: 'timer',
    power: 'economy',
  });
  assert.deepEqual(parseStatus(null).errors, {});
});

test('heightFromLabel', () => {
  assert.equal(heightFromLabel('Punkt 3 (450 cm)'), 450);
  assert.equal(heightFromLabel('P2 1,5 m'), 150);
  assert.equal(heightFromLabel('Punkt 1'), null);
  assert.equal(heightFromLabel(undefined), null);
});

test('parseFeed sorts entries and keeps nulls', () => {
  const f = parseFeed({
    channel: { name: 'X', field1: 'Punkt 1 (100 cm)' },
    feeds: [
      { created_at: '2026-11-20T02:00:00Z', field1: '2.5', field2: null },
      { created_at: '2026-11-20T01:00:00Z', field1: '0.00' },
      { created_at: 'garbage', field1: '9' },
    ],
  });
  assert.equal(f.entries.length, 2);
  assert.ok(f.entries[0].t < f.entries[1].t);
  assert.equal(f.entries[0].values[0], 0);
  assert.equal(f.entries[1].values[1], null);
  assert.equal(f.fields[0].heightCm, 100);
});

test('latestPoints: ok, error reason, missing and not mapped', () => {
  const f = feed(3, (h) => ({
    field1: '12.5',
    field2: h === 2 ? null : '13',
    field3: h === 2 ? null : '14',
    status: h === 2 ? 'seq=3 p2:crc_error' : `seq=${h + 1}`,
  }));
  const p = latestPoints(f);
  assert.equal(p[0].status, 'ok');
  assert.equal(p[0].temp_c, 12.5);
  assert.equal(p[1].status, 'crc_error');
  assert.equal(p[1].temp_c, null);
  assert.equal(p[2].status, 'missing'); // had data before, no reason now
  assert.equal(p[7].status, 'not_mapped'); // never had data
});

test('pointStats and change24h', () => {
  const f = feed(30, (h) => ({ field1: String(10 + h * 0.1), field2: h % 2 ? null : '5' }));
  const s = pointStats(f, 0);
  assert.equal(s.count, 30);
  assert.equal(s.min, 10);
  assert.ok(Math.abs(s.max - 12.9) < 1e-9);
  assert.ok(Math.abs(change24h(f, 0) - 2.4) < 1e-9);
  assert.equal(pointStats(f, 5).count, 0);
  assert.equal(
    change24h(
      feed(5, () => ({ field1: '1' })),
      0,
    ),
    null,
  ); // < 24 h of data
});

test('alerts: hot, rising, sensor and stale', () => {
  const f = feed(30, (h) => ({
    field1: String(15 + h * 0.2), // +4.8 in 24 h
    field2: '31',
    field3: h === 29 ? null : '10',
    status: h === 29 ? 'seq=30 p3:missing' : '',
  }));
  const now = f.entries[f.entries.length - 1].t + 10 * 60000;
  const a = alerts(f, { maxTemp: 30, risePerDay: 3, staleMinutes: 150 }, now);
  assert.ok(a.some((x) => x.kind === 'rising' && x.point === 1));
  assert.ok(a.some((x) => x.kind === 'hot' && x.point === 2));
  assert.ok(a.some((x) => x.kind === 'sensor' && x.point === 3 && x.value === 'missing'));
  assert.ok(!a.some((x) => x.kind === 'stale'));
  const later = alerts(f, { maxTemp: null, risePerDay: 0, staleMinutes: 150 }, now + 3 * H);
  assert.deepEqual(
    later.map((x) => x.kind),
    ['stale', 'sensor'],
  );
});

test('segments break at missing values and outages, never through 0', () => {
  const f = parseFeed({
    feeds: [
      { created_at: iso(T0), field1: '1' },
      { created_at: iso(T0 + H), field1: '2' },
      { created_at: iso(T0 + 2 * H), field1: null },
      { created_at: iso(T0 + 3 * H), field1: '3' },
      { created_at: iso(T0 + 10 * H), field1: '4' }, // 7 h outage
    ],
  });
  const gap = gapThreshold(f);
  assert.ok(gap >= 45 * 60000);
  const s = segments(f, 0, gap);
  assert.equal(s.length, 3);
  assert.deepEqual(
    s.map((x) => x.length),
    [2, 1, 1],
  );
  assert.ok(s.flat().every(([, v]) => v !== 0));
});

test('downsample keeps time order and bounds size', () => {
  const seg = Array.from({ length: 3000 }, (_, k) => [k * 1000, k % 10]);
  const d = downsample(seg, 300);
  assert.ok(d.length <= 300);
  for (let k = 1; k < d.length; k++) assert.ok(d[k][0] > d[k - 1][0]);
  assert.equal(downsample(seg.slice(0, 10), 300).length, 10);
});

test('niceTicks', () => {
  assert.deepEqual(niceTicks(12.3, 17.8, 5), [12, 13, 14, 15, 16, 17, 18]);
  assert.deepEqual(niceTicks(-3, 22, 5), [-5, 0, 5, 10, 15, 20, 25]);
  assert.ok(niceTicks(5, 5).length >= 2);
  assert.deepEqual(niceTicks(NaN, 1), []);
});

test('thingSpeakTime is UTC', () => {
  assert.equal(thingSpeakTime(Date.parse('2026-11-23T05:07:09Z')), '2026-11-23 05:07:09');
});

test('latestDiag reads the diagnostic channel layout', () => {
  const f = feed(2, () => ({
    field1: '3912',
    field2: '-61',
    field3: '8',
    field5: '1',
    field8: '120',
    status: 'fw=0.1.1 wake=timer power=normal',
  }));
  const d = latestDiag(f);
  assert.equal(d.batteryMv, 3912);
  assert.equal(d.rssi, -61);
  assert.equal(d.sensorErrors, null);
  assert.equal(d.firmware, '0.1.1');
  assert.equal(d.power, 'normal');
  assert.equal(latestDiag(parseFeed({ feeds: [] })), null);
});

test('parseHistoryBinary: device export -> same shape as ThingSpeak feeds', async () => {
  const { parseHistoryBinary, segments, pointStats } = await import('../src/lib/feed.js');
  const rows = 3;
  const buf = new ArrayBuffer(8 + rows * 20);
  const dv = new DataView(buf);
  'GMH1'.split('').forEach((c, i) => dv.setUint8(i, c.charCodeAt(0)));
  dv.setUint16(4, rows, true);
  dv.setUint16(6, 8, true);
  const base = Date.parse('2026-11-20T00:00:00Z') / 1000;
  for (let k = 0; k < rows; k++) {
    const o = 8 + k * 20;
    dv.setUint32(o, base + (rows - 1 - k) * 900, true); // reverse order on purpose
    for (let i = 0; i < 8; i++) dv.setInt16(o + 4 + 2 * i, i === 2 && k === 1 ? -32768 : 1000 + k * 10 + i, true);
  }
  const f = parseHistoryBinary(buf, [{ label: 'P1 (150 cm)', heightCm: 150 }]);
  assert.equal(f.entries.length, 3);
  assert.ok(f.entries[0].t < f.entries[2].t);
  assert.equal(f.entries[2].values[0], 10); // first row (k=0) is the newest
  assert.equal(f.entries[1].values[2], null); // missing, not 0
  assert.equal(f.fields[0].heightCm, 150);
  assert.equal(f.fields[1].heightCm, null);
  assert.equal(pointStats(f, 0).count, 3);
  assert.equal(segments(f, 2, 3600000).length, 2);
  assert.throws(() => parseHistoryBinary(new ArrayBuffer(4)));
});
