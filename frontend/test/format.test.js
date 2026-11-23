import { test } from 'node:test';
import assert from 'node:assert/strict';
import { formatDuration, formatClock, formatRom, samplesPerUpload, rssiBars } from '../src/lib/format.js';
import { inspectImage } from '../src/lib/ota.js';

test('durations', () => {
  assert.equal(formatDuration(42), '42 s');
  assert.equal(formatDuration(3700), '1 h 1 min');
  assert.equal(formatDuration(900), '15 min');
  assert.equal(formatDuration(90061), '1 d 1 h');
  assert.equal(formatDuration(null), '—');
  assert.equal(formatClock(65), '01:05');
  assert.equal(formatClock(3725), '1:02:05');
});

test('ROM formatting keeps all 64 bits', () => {
  assert.equal(formatRom('28ff64851e160304'), '28 FF 64 85 1E 16 03 04');
  assert.equal(formatRom('bogus'), 'bogus');
});

test('samples per upload matches firmware rule', () => {
  assert.equal(samplesPerUpload(900, 3600), 4);
  assert.equal(samplesPerUpload(900, 1000), 1);
  assert.equal(samplesPerUpload(0, 3600), null);
});

test('rssi bars', () => {
  assert.equal(rssiBars(0), 0);
  assert.equal(rssiBars(-50), 4);
  assert.equal(rssiBars(-95), 0);
});

test('OTA header pre-check', () => {
  const b = new Uint8Array(128);
  b[0] = 0xe9;
  b[12] = 0x0d;
  b.set([0x32, 0x54, 0xcd, 0xab], 32);
  b.set(new TextEncoder().encode('1.2.3'), 48);
  b.set(new TextEncoder().encode('grainmeter'), 80);
  assert.deepEqual(inspectImage(b), { ok: true, reason: undefined, version: '1.2.3', project: 'grainmeter' });
  b[12] = 0x05;
  assert.equal(inspectImage(b).reason, 'chip');
  assert.equal(inspectImage(new Uint8Array(10)).reason, 'short');
});
