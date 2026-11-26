import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { build } from 'esbuild';
import { parseFeed, latestDiag } from '../src/lib/feed.js';
import { translate } from '../src/i18n/translate.js';

const root = fileURLToPath(new URL('..', import.meta.url));
const bundle = await build({
  absWorkingDir: root,
  stdin: {
    contents:
      "export { Overview } from './src/pages/Overview.jsx'; export { DeviceCard } from './src/dashboard/app.jsx';",
    resolveDir: root,
  },
  bundle: true,
  write: false,
  format: 'esm',
  platform: 'node',
  jsx: 'automatic',
  jsxImportSource: 'preact',
  plugins: [
    {
      name: 'runtime-fixtures',
      setup(b) {
        b.onLoad({ filter: /components\/hooks\.js$/ }, () => ({
          contents: `export const useStatus = () => ({status: globalThis.runtimeStatus});
          export const useTick = () => {};
          export const useLoad = () => ({});
          export const useAction = () => ({});`,
          loader: 'js',
        }));
        b.onLoad({ filter: /components\/i18n\.js$/ }, () => ({
          contents: `export const useI18n = () => globalThis.runtimeI18n;
          export const tOr = (t, prefix, value) => t(prefix + value);
          export const errorText = () => '';
          export const I18nContext = {};`,
          loader: 'js',
        }));
        b.onLoad({ filter: /dashboard\/app\.jsx$/ }, ({ path: p }) => ({
          contents: readFileSync(p, 'utf8') + '\nexport { DeviceCard };',
          loader: 'jsx',
          resolveDir: path.dirname(p),
        }));
      },
    },
  ],
});
const { Overview, DeviceCard } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);

// Read the real JSX tree without mounting charts or a browser DOM.
function text(node) {
  if (node === null || node === undefined || typeof node === 'boolean') return '';
  if (Array.isArray(node)) return node.map(text).join(' ');
  if (typeof node !== 'object') return String(node);
  if (typeof node.type === 'function') {
    if (['SiloView', 'TempLegend', 'DeviceCharts'].includes(node.type.name)) return '';
    return text(node.type(node.props));
  }
  return text(node.props.children);
}

function diag(status, mv = 3840) {
  return parseFeed({ feeds: [{ created_at: '2026-11-26T12:00:00Z', field1: mv, status }] });
}

test('Overview renders runtime and keeps voltage forecast, in pl/en', () => {
  try {
    for (const lang of ['pl', 'en']) {
      const t = (k, p) => translate(lang, k, p);
      globalThis.runtimeI18n = { lang, t };
      globalThis.runtimeStatus = {
        config_ok: true,
        upload_configured: true,
        battery_mv: 3840,
        battery_runtime: { mah_per_day: 3.5, days: 357.14 },
      };
      const rendered = text(Overview());
      assert.ok(rendered.includes(t('batteryRuntime.text', { days: 357, mah: '3.50' })));
      assert.ok(rendered.includes(t('batteryForecast.unknown')));
      globalThis.runtimeStatus.battery_runtime.days = null;
      assert.ok(text(Overview()).includes(t('batteryRuntime.collecting')));
      delete globalThis.runtimeStatus.battery_runtime;
      assert.ok(text(Overview()).includes(t('batteryRuntime.collecting')));
    }
  } finally {
    delete globalThis.runtimeStatus;
    delete globalThis.runtimeI18n;
  }
});

test('dashboard renders diagnostic rt or computes from mah and capacity', () => {
  for (const lang of ['pl', 'en']) {
    const t = (k, p) => translate(lang, k, p);
    const render = (feed) => text(DeviceCard({ diag: feed, t, now: Date.parse('2026-11-26T12:00:00Z'), range: '7d' }));
    assert.ok(render(diag('fw=1.0 mah=3.50 rt=12.00')).includes(t('batteryRuntime.text', { days: 12, mah: '3.50' })));
    assert.ok(render(diag('mah=3.50')).includes(t('batteryRuntime.text', { days: 357, mah: '3.50' })));
    assert.ok(render(diag('mah=3.50 capacity=5000')).includes(t('batteryRuntime.text', { days: 714, mah: '3.50' })));
    assert.ok(render(diag('mah=3.50 rt=0.00')).includes(t('batteryRuntime.text', { days: 0, mah: '3.50' })));
    for (const status of ['', 'mah=0', 'mah=-1', 'mah=bad', 'rt=12'])
      assert.ok(render(diag(status)).includes(t('batteryRuntime.collecting')));
    assert.ok(render(diag('mah=3.50', null)).includes(t('batteryRuntime.collecting')));
    assert.ok(render(diag('mah=3.50 capacity=99')).includes(t('batteryRuntime.collecting')));
  }
});

test('battery-only rows keep the newest complete diagnostic, not older known consumption', () => {
  const feed = parseFeed({
    feeds: [
      { created_at: '2026-11-26T10:00:00Z', field1: 3840, status: 'mah=3.50 rt=357.14' },
      { created_at: '2026-11-26T11:00:00Z', field1: 3800 },
    ],
  });
  assert.deepEqual(latestDiag(feed).batteryRuntime, { mah_per_day: 3.5, days: 357.14 });
  feed.entries.push(...diag('fw=1.0 capacity=2500').entries);
  assert.deepEqual(latestDiag(feed).batteryRuntime, { mah_per_day: null, days: null });
  assert.equal(latestDiag(diag('mah=3.50 capacity=bad')).batteryRuntime.days, null);
});
