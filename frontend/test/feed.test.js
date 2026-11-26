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

// Builds a GMH3 page: rows = [{t (s), c (centi for all points), mv, rssi, flags}]
function gmh3(rows, next = 0, maxGap = 12600) {
  const buf = new ArrayBuffer(16 + rows.length * 24);
  const dv = new DataView(buf);
  'GMH3'.split('').forEach((ch, i) => dv.setUint8(i, ch.charCodeAt(0)));
  dv.setUint16(4, rows.length, true);
  dv.setUint16(6, 8, true);
  dv.setUint32(8, next, true);
  dv.setUint32(12, maxGap, true);
  rows.forEach((r, k) => {
    const o = 16 + k * 24;
    dv.setUint32(o, r.t, true);
    for (let i = 0; i < 8; i++) dv.setInt16(o + 4 + 2 * i, r.missing === i ? -32768 : r.c + i, true);
    dv.setUint16(o + 20, r.mv || 0, true);
    dv.setInt8(o + 22, r.rssi || 0);
    dv.setUint8(o + 23, r.flags || 0);
  });
  return buf;
}

const H0 = Date.parse('2026-11-20T00:00:00Z') / 1000;

test('parseHistoryBinary: GMH3 page with device state', async () => {
  const { parseHistoryBinary, segments, pointStats, gapThreshold } = await import('../src/lib/feed.js');
  const f = parseHistoryBinary(
    gmh3(
      [
        { t: H0 + 1800, c: 1010, mv: 3500, rssi: -79, flags: 1 | (2 << 2), missing: 2 },
        { t: H0, c: 1000, mv: 3900, rssi: -61, flags: 1 << 2 },
        { t: H0 + 9000, c: 1020, flags: 3 | 0x10 | 0x20 },
        { t: H0 + 9900, c: 1020, mv: 3890, rssi: -60, flags: 0x40 },
      ],
      H0 + 10000,
    ),
    [{ label: 'P1 (150 cm)', heightCm: 150 }],
  );
  assert.equal(f.entries.length, 4);
  assert.equal(f.entries[0].t, H0 * 1000); // sorted
  assert.equal(f.entries[1].values[0], 10.1);
  assert.equal(f.entries[1].values[2], null); // missing, not 0
  assert.deepEqual(f.entries[1].diag, {
    batteryMv: 3500,
    rssi: -79,
    power: 'economy',
    upload: 'failed',
    reconstructed: false,
    hold: false,
  });
  assert.equal(f.entries[2].restart, true);
  assert.equal(f.entries[2].diag.batteryMv, null);
  assert.equal(f.entries[2].diag.power, null);
  assert.equal(f.entries[3].diag.hold, true);
  assert.equal(f.next, H0 + 10000);
  assert.equal(f.maxGapMs, 12600000);
  assert.equal(gapThreshold(f), 12600000);
  assert.equal(f.fields[0].heightCm, 150);
  assert.equal(pointStats(f, 0).count, 4);
  // a restart (power-up) breaks the line even within maxGap
  assert.equal(segments(f, 0, gapThreshold(f)).length, 2);
  assert.throws(() => parseHistoryBinary(new ArrayBuffer(8)));
});

test('mergeFeeds: days joined, duplicates and superseded hold rows dropped', async () => {
  const { parseHistoryBinary, mergeFeeds } = await import('../src/lib/feed.js');
  const day1 = parseHistoryBinary(
    gmh3([
      { t: H0, c: 1000 },
      { t: H0 + 900, c: 1100, flags: 0x40 },
    ]),
  );
  const day2 = parseHistoryBinary(
    gmh3([
      { t: H0 + 900, c: 1100 },
      { t: H0 + 1800, c: 1200 },
    ]),
  );
  const m = mergeFeeds([day2, day1]);
  assert.deepEqual(
    m.entries.map((e) => e.t / 1000 - H0),
    [0, 900, 1800],
  );
  assert.equal(m.maxGapMs, 12600000);
  const held = mergeFeeds([
    parseHistoryBinary(
      gmh3([
        { t: H0, c: 1 },
        { t: H0 + 500, c: 1, flags: 0x40 },
        { t: H0 + 900, c: 2 },
      ]),
    ),
  ]);
  assert.deepEqual(
    held.entries.map((e) => e.t / 1000 - H0),
    [0, 900],
  );
});

test('dayChunks + loadHistory: newest day first, pages, cache of past days', async () => {
  const { dayChunks, loadHistory, historyCapacityDays } = await import('../src/lib/historyLoader.js');
  const now = new Date(2026, 10, 20, 15, 30).getTime(); // local time
  const chunks = dayChunks(now, 7 * 86400000);
  assert.equal(chunks.length, 8); // 7 days back to midnight + today
  // today and yesterday may still receive backfilled samples: not cached
  assert.equal(chunks[0].complete, false);
  assert.equal(chunks[1].complete, false);
  assert.ok(chunks.slice(3).every((c) => c.complete));
  assert.ok(chunks[0].from > chunks[1].from);
  assert.equal(new Date(chunks[1].from * 1000).getHours(), 0);
  assert.equal(chunks[1].to + 1, chunks[0].from);

  const calls = [];
  const paged = new Set();
  const fetchPage = async (from, to) => {
    calls.push(from);
    // two pages for each day: the first one says where to continue
    if (!paged.has(to)) {
      paged.add(to);
      return gmh3([{ t: from, c: 1000 }], from + 3600);
    }
    return gmh3([{ t: from + 60, c: 1001 }]);
  };
  const cache = new Map();
  const progress = [];
  const f = await loadHistory({
    nowMs: now,
    rangeMs: 3 * 86400000,
    fetchPage,
    cache,
    onProgress: (_feed, done, total) => progress.push(`${done}/${total}`),
  });
  assert.deepEqual(progress, ['1/4', '2/4', '3/4', '4/4']);
  assert.equal(calls.length, 8); // 4 days x 2 pages
  assert.equal(f.entries.length, 8);
  assert.equal(cache.size, 1); // only the day older than the backfill window
  calls.length = 0;
  paged.clear();
  await loadHistory({ nowMs: now, rangeMs: 3 * 86400000, fetchPage, cache });
  assert.equal(calls.length, 6); // 3 recent days again, the oldest from the cache

  assert.equal(
    historyCapacityDays({
      records: 10,
      used_bytes: 1000,
      capacity_bytes: 100000,
      oldest_epoch: H0,
      newest_epoch: H0 + 2 * 86400,
    }),
    200,
  );
  assert.equal(historyCapacityDays({ records: 0 }), null);
});

test('deviceSummary: time-weighted power modes, failed upload periods', async () => {
  const { parseHistoryBinary, deviceFeed, deviceSummary } = await import('../src/lib/feed.js');
  const f = parseHistoryBinary(
    gmh3([
      { t: H0, c: 1500, mv: 3900, rssi: -61, flags: 0 | (1 << 2) }, // normal, ok: 3 h
      { t: H0 + 10800, c: 1500, mv: 3500, rssi: -79, flags: 1 | (2 << 2) }, // economy, failed: 1 h
      { t: H0 + 14400, c: 1500, mv: 3480, rssi: -80, flags: 1 | (2 << 2) }, // still failed
      { t: H0 + 14400 + 3600, c: 1500, mv: 3470, rssi: -70, flags: 1 | (1 << 2) },
    ]),
  );
  assert.deepEqual(deviceFeed(f).entries[0].values, [3.9, -61]);
  const s = deviceSummary(f);
  assert.equal(s.batteryMin, 3470);
  assert.equal(s.batteryMax, 3900);
  assert.equal(s.batteryLast, 3470);
  assert.equal(s.rssiWeakest, -80);
  assert.equal(s.failedUploads, 1);
  assert.equal(s.economyPct, 40); // 2 h of 5 h
  assert.equal(s.criticalPct, 0);
  assert.equal(deviceSummary({ entries: [] }), null);
});

test('percent views: battery from the LiPo curve, signal -90..-50 dBm', async () => {
  const { rssiPercent } = await import('../src/lib/format.js');
  const { devicePercentFeed } = await import('../src/lib/feed.js');
  assert.equal(rssiPercent(-50), 100);
  assert.equal(rssiPercent(-40), 100);
  assert.equal(rssiPercent(-70), 50);
  assert.equal(rssiPercent(-90), 0);
  assert.equal(rssiPercent(-95), 0);
  assert.equal(rssiPercent(null), null);
  assert.equal(rssiPercent(0), null);
  const f = devicePercentFeed({
    entries: [
      { t: 1, values: [3.7, -70] },
      { t: 2, values: [null, null] },
    ],
  });
  assert.deepEqual(f.entries[0].values, [13, 50]); // 3.70 V: between 3.69 V (10 %) and 3.71 V (15 %)
  assert.deepEqual(f.entries[1].values, [null, null]);
});
