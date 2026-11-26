import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseFeed } from '../src/lib/feed.js';
import { collectChanges, DATA_GAP_MS, visibleChanges } from '../src/dashboard/changes.js';

const T0 = Date.parse('2026-11-20T00:00:00Z');
const H = 3600 * 1000;
const feed = (rows) =>
  parseFeed({ feeds: rows.map((r, i) => ({ created_at: new Date(T0 + i * H).toISOString(), ...r })) });

test('temperature threshold uses the previous reading, including zero and recovery after errors', () => {
  const main = feed([
    { field1: 0 },
    { field1: 0.3 },
    { field1: 0.7 },
    { field1: 1.2 },
    { status: 'p1:crc_error' },
    { status: 'p1:missing' },
    { field1: 0.7 },
    {},
    { field1: 0.8 },
  ]);
  const original = JSON.stringify(main);
  const changes = collectChanges(main, null);
  assert.deepEqual(
    changes.filter((c) => c.kind === 'temperature').map((c) => [c.before, c.after]),
    [
      [1.2, 0.7],
      [0.7, 1.2],
    ],
  );
  assert.deepEqual(
    changes.filter((c) => c.kind === 'sensor').map((c) => [c.before, c.after]),
    [
      ['missing', 'ok'],
      ['ok', 'missing'],
      ['missing', 'ok'],
      ['crc_error', 'missing'],
      ['ok', 'crc_error'],
    ],
  );
  assert.ok(changes.every((c) => c.point === 1));
  assert.equal(JSON.stringify(main), original);
});

test('initial absent points are not events; explicit sensor errors and alarm transitions are', () => {
  const main = feed([
    { status: 'p3:crc_error alarm=temp' },
    { status: 'p3:crc_error alarm=temp' },
    { field3: 12, status: 'alarm=rise' },
    { field3: 12 },
  ]);
  const changes = collectChanges(main, null);
  assert.deepEqual(
    changes.filter((c) => c.kind === 'alarm').map((c) => [c.before, c.after]),
    [
      ['rise', null],
      ['temp', 'rise'],
      [null, 'temp'],
    ],
  );
  assert.deepEqual(
    changes.filter((c) => c.kind === 'sensor').map((c) => [c.point, c.before, c.after]),
    [
      [3, 'crc_error', 'ok'],
      [3, null, 'crc_error'],
    ],
  );
});

test('a point first appearing later in the range is a reading-state change, not a temperature delta', () => {
  const changes = collectChanges(feed([{}, { field2: 0 }, { field2: 0 }]), null);
  assert.deepEqual(changes, [
    { t: T0 + H, type: 'temperature', kind: 'sensor', before: 'missing', after: 'ok', point: 2 },
  ]);
});

test('battery buckets, mode, firmware and retained counter resets ignore sparse diagnostic rows', () => {
  const diag = feed([
    { field1: 3800, field5: 9, field8: 100, status: 'fw=1.0 power=normal wake=timer' },
    { field1: 3820 },
    { field1: null },
    { field1: 3840, field5: 0, field8: 104, status: 'fw=1.0 power=economy wake=timer' },
    { field1: 3850 },
    { field1: 3800, field8: 1, status: 'fw=1.1 power=critical wake=timer' },
    { field1: 0 },
  ]);
  const changes = collectChanges(null, diag);
  assert.deepEqual(
    changes.filter((c) => c.kind === 'battery').map((c) => [c.before, c.after]),
    [
      [55, 40],
      [45, 50],
    ],
  );
  assert.deepEqual(
    changes.filter((c) => c.kind === 'power').map((c) => [c.before, c.after]),
    [
      ['economy', 'critical'],
      ['normal', 'economy'],
    ],
  );
  assert.deepEqual(
    changes.filter((c) => c.kind === 'firmware').map((c) => [c.before, c.after]),
    [['1.0', '1.1']],
  );
  assert.deepEqual(
    changes.filter((c) => c.kind === 'counterReset').map((c) => [c.before, c.after]),
    [[104, 1]],
  );
  assert.ok(!changes.some((c) => c.kind === 'charging' || c.kind === 'restart'));
});

test('data gaps use both channels, strict 2.5 h threshold, no invented range-edge events', () => {
  const main = feed([
    {},
    { created_at: new Date(T0 + DATA_GAP_MS).toISOString() },
    { created_at: new Date(T0 + 8 * H).toISOString() },
  ]);
  const diag = feed([{ created_at: new Date(T0 + 4 * H).toISOString(), field1: 3900 }]);
  const gaps = collectChanges(main, diag);
  assert.equal(gaps.length, 1);
  assert.deepEqual(gaps[0], { t: T0 + 8 * H, type: 'device', kind: 'gap', before: T0 + 4 * H, after: T0 + 8 * H });
  assert.deepEqual(collectChanges(null, null), []);
  assert.deepEqual(collectChanges(feed([{}]), null), []);
});

test('changes are newest first and the 200-row limit is applied after filtering', () => {
  const main = feed(Array.from({ length: 220 }, (_, i) => ({ field1: i })));
  const diag = feed([{ status: 'fw=1' }, { status: 'fw=2' }]);
  const changes = collectChanges(main, diag);
  assert.ok(changes.every((c, i) => i === 0 || c.t <= changes[i - 1].t));
  assert.equal(visibleChanges(changes).total, 220);
  assert.equal(visibleChanges(changes).rows.length, 200);
  assert.equal(visibleChanges(changes).rows[0].after, 219);
  assert.equal(visibleChanges(changes, 'temperature').total, 219);
  assert.equal(visibleChanges(changes, 'device').total, 1);
  assert.equal(visibleChanges(changes, 'device').rows[0].kind, 'firmware');
  assert.deepEqual(visibleChanges(changes, 'battery'), { rows: [], total: 0 });
});
