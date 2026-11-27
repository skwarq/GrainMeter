import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { build } from 'esbuild';
import { latestWindowMs, predictWake } from '../src/lib/wake.js';
import { parseFeed } from '../src/lib/feed.js';
import { translate } from '../src/i18n/translate.js';
import { loadDeviceSchedule, rememberFromConfig, rememberFromStatus } from '../src/lib/device-schedule.js';

const MINUTE = 60000;
const HOUR = 60 * MINUTE;
const START = Date.parse('2026-11-27T08:00:00Z');
const normal = {
  lastWindowMs: START,
  uploadS: 3600,
  measureS: 900,
  power: 'normal',
  nowMs: START + 10 * MINUTE,
};

test('normal: three future windows, RTC margin and advance preparation', () => {
  const p = predictWake(normal);
  assert.equal(p.state, 'ok');
  assert.deepEqual(p.windows, [START + HOUR, START + 2 * HOUR, START + 3 * HOUR]);
  assert.equal(p.prepareByMs, START + HOUR - 3 * MINUTE);
  assert.equal(p.marginMs, 2 * MINUTE);
  assert.equal(p.uncertain, false);
});

test('non-divisible upload intervals round up to measurement wakes, including economy', () => {
  assert.equal(predictWake({ ...normal, uploadS: 1000 }).intervalMs, 1800000);
  const p = predictWake({ ...normal, uploadS: 1000, power: 'economy', count: 2 });
  assert.equal(p.intervalMs, 4500000); // ceil(4000 / 900) * 900, not 4 * ceil(1000 / 900) * 900
  assert.deepEqual(p.windows, [START + 4500000, START + 9000000]);
  assert.equal(predictWake({ ...normal, power: 'economy' }).intervalMs, 4 * HOUR);
});

test('critical: no radio windows, even without configuration or reports', () => {
  const p = predictWake({ power: 'critical' });
  assert.equal(p.state, 'critical');
  assert.deepEqual(p.windows, []);
  assert.equal(p.prepareByMs, null);
});

test('overdue: roll forward by whole intervals, never predict a past window', () => {
  const p = predictWake({ ...normal, nowMs: START + 5 * HOUR + 20 * MINUTE });
  assert.equal(p.state, 'overdue');
  assert.equal(p.uncertain, true);
  assert.deepEqual(p.windows, [START + 6 * HOUR, START + 7 * HOUR, START + 8 * HOUR]);
});

test('elapsed prediction is uncertain; overdue only after the RTC margin', () => {
  const p = predictWake({ ...normal, nowMs: START + HOUR });
  assert.equal(p.state, 'ok');
  assert.equal(p.uncertain, true);
  assert.equal(p.windows[0], START + 2 * HOUR);
  assert.equal(predictWake({ ...normal, nowMs: START + HOUR + 2 * MINUTE }).state, 'ok');
  assert.equal(predictWake({ ...normal, nowMs: START + HOUR + 2 * MINUTE + 1 }).state, 'overdue');
});

test('missing, invalid or far-future data do not invent a schedule', () => {
  for (const change of [
    { lastWindowMs: null },
    { uploadS: null },
    { measureS: null },
    { measureS: 0 },
    { uploadS: -1 },
    { nowMs: NaN },
    { lastWindowMs: normal.nowMs + 3 * MINUTE },
  ]) {
    const p = predictWake({ ...normal, ...change });
    assert.equal(p.state, 'unknown');
    assert.deepEqual(p.windows, []);
  }
  assert.equal(predictWake({}).state, 'unknown');
});

test('last window uses the newer channel, including battery-only diagnostic rows', () => {
  const main = { entries: [{ t: START }] };
  const diag = { entries: [{ t: START + HOUR }] };
  assert.equal(latestWindowMs(main, diag), START + HOUR);
  assert.equal(latestWindowMs(diag, main), START + HOUR);
  assert.equal(latestWindowMs(main, null), START);
  assert.equal(latestWindowMs(null, diag), START + HOUR);
  assert.equal(latestWindowMs({ entries: [] }, null), null);
  assert.equal(latestWindowMs(null, null), null);
  assert.equal(predictWake({ ...normal, power: null }).uncertain, true);
});

// Exercise the actual JSX in both languages without a browser or chart mounting.
const bundle = await build({
  stdin: {
    contents: readFileSync(new URL('../src/dashboard/app.jsx', import.meta.url), 'utf8') + '\nexport { WakeCard };',
    resolveDir: new URL('../src/dashboard/', import.meta.url).pathname,
    loader: 'jsx',
  },
  bundle: true,
  write: false,
  format: 'esm',
  platform: 'node',
  jsx: 'automatic',
  jsxImportSource: 'preact',
});
const { WakeCard } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);
function text(node) {
  if (node == null || typeof node === 'boolean') return '';
  if (Array.isArray(node)) return node.map(text).join(' ');
  if (typeof node !== 'object') return String(node);
  if (typeof node.type === 'function') return text(node.type(node.props));
  return text(node.props.children);
}
function diagnostic(status) {
  return parseFeed({ feeds: [{ created_at: new Date(START).toISOString(), field1: 3800, status }] });
}

test('wake card renders predictions and LAN/MQTT preparation, with disabled capabilities', () => {
  for (const lang of ['pl', 'en']) {
    const t = (k, p) => translate(lang, k, p);
    const props = { now: normal.nowMs, lang, t };
    const render = (status) => text(WakeCard({ ...props, diag: diagnostic(status) }));
    const enabled = render('power=normal up=3600 ms=900 lan=1 tb=1');
    assert.match(enabled, new RegExp(t('wake.at', { time: '.+', left: '50 min' }).replace(/[()]/g, '\\$&')));
    assert.ok(enabled.includes('make lan-console'));
    assert.ok(enabled.includes(t('wake.remoteWake')));
    assert.ok(enabled.includes(t('wake.lanHint')));
    assert.ok(enabled.includes(t('wake.mqttHint')));
    assert.ok(enabled.includes(t('wake.caution')));
    assert.equal(enabled.includes(t('wake.estimate')), false);
    const disabled = render('power=economy up=3600 ms=900 lan=0 tb=0');
    assert.ok(disabled.includes(t('wake.lanOff')));
    assert.ok(disabled.includes(t('wake.mqttOff')));
    assert.ok(disabled.includes(t('power.economy')));
    assert.equal(disabled.includes('make lan-console'), false);
  }
});

test('wake card renders critical and unknown states, and a main-only legacy estimate', () => {
  for (const lang of ['pl', 'en']) {
    const t = (k, p) => translate(lang, k, p);
    const props = { now: normal.nowMs, lang, t };
    const critical = text(WakeCard({ ...props, diag: diagnostic('power=critical up=3600 ms=900') }));
    assert.ok(critical.includes(t('wake.critical')));
    assert.equal(critical.includes(t('wake.following')), false);
    assert.equal(critical.includes('make lan-console'), false);
    assert.equal(critical.includes(t('wake.remoteWake')), false);
    assert.ok(text(WakeCard(props)).includes(t('wake.unknown')));
    const legacy = text(
      WakeCard({
        ...props,
        main: diagnostic(''),
        schedule: { upload_s: 3600, measure_s: 900, savedAt: START },
      }),
    );
    assert.ok(legacy.includes(t('wake.following')));
    assert.ok(
      legacy.includes(
        t('wake.remembered', { date: new Date(START).toLocaleDateString(lang === 'en' ? 'en-GB' : 'pl-PL') }),
      ),
    );
    assert.ok(legacy.includes('make lan-console'));
    assert.ok(legacy.includes(t('wake.remoteWake')));
  }
});

test('remote portal remembers the schedule for the dashboard; diagnostic status wins', () => {
  const store = new Map();
  const storage = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, v) };
  rememberFromStatus({ schedule: { upload_s: 3600, measure_s: 900 } }, storage);
  rememberFromConfig({ upload_s: 3600, measure_s: 900, lan_wake: false, talkback_id: 0 }, storage);
  const mem = loadDeviceSchedule(storage);
  assert.deepEqual(
    { ...mem, savedAt: 0 },
    { upload_s: 3600, measure_s: 900, lan_wake: false, talkback: false, savedAt: 0 },
  );
  rememberFromStatus({ schedule: { upload_s: 'x' } }, storage);
  assert.equal(loadDeviceSchedule(storage).upload_s, 3600);
  assert.equal(loadDeviceSchedule({ getItem: () => '{bad' }), null);
  const t = (k, p) => translate('pl', k, p);
  const props = { now: normal.nowMs, lang: 'pl', t, schedule: { ...mem, upload_s: 1800 } };
  const fromMem = text(WakeCard({ ...props, main: diagnostic('') }));
  assert.ok(fromMem.includes(t('wake.lanOff')));
  assert.ok(fromMem.includes('za 20 min'));
  const fromDiag = text(WakeCard({ ...props, diag: diagnostic('power=normal up=3600 ms=900 lan=1 tb=1') }));
  assert.ok(fromDiag.includes('za 50 min'));
  assert.equal(fromDiag.includes(t('wake.remembered', { date: '' }).slice(0, 10)), false);
});
