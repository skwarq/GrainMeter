import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { parseFeed, RANGES } from '../src/lib/feed.js';
import { translate } from '../src/i18n/translate.js';

const bundle = await build({
  stdin: {
    contents: `export { App } from './src/dashboard/app.jsx';
      export { HistoryCard, DeviceCharts } from './src/components/History.jsx';
      export { LineChart } from './src/components/LineChart.jsx';`,
    resolveDir: new URL('..', import.meta.url).pathname,
  },
  bundle: true,
  write: false,
  format: 'esm',
  platform: 'node',
  jsx: 'automatic',
  jsxImportSource: 'preact',
  plugins: [
    {
      name: 'history-view-fixtures',
      setup(b) {
        b.onResolve({ filter: /^preact\/hooks$/ }, () => ({ path: 'hooks', namespace: 'fixtures' }));
        b.onLoad({ filter: /^hooks$/, namespace: 'fixtures' }, () => ({
          contents: `export const useState = (initial) => {
            const h = globalThis.historyHooks;
            const i = h.index++;
            if (!(i in h.values)) h.values[i] = typeof initial === 'function' ? initial() : initial;
            return [h.values[i], (v) => { h.values[i] = v; }];
          };
          export const useMemo = (fn) => fn();
          export const useContext = () => globalThis.historyI18n;
          export const useEffect = () => {};
          export const useCallback = (fn) => fn;
          export const useRef = (v) => ({current: v});`,
        }));
      },
    },
  ],
});
const { App, HistoryCard, DeviceCharts, LineChart } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);

function nodes(tree) {
  if (!tree || typeof tree !== 'object') return [];
  if (Array.isArray(tree)) return tree.flatMap(nodes);
  return [tree, ...nodes(tree.props?.children)];
}

function fixture(lang = 'pl') {
  const storage = new Map([['gm.lang', lang]]);
  const previousStorage = Object.getOwnPropertyDescriptor(globalThis, 'localStorage');
  globalThis.localStorage = {
    getItem: (key) => storage.get(key) ?? null,
    setItem: (key, value) => storage.set(key, value),
  };
  globalThis.historyHooks = { index: 0, values: [] };
  globalThis.historyI18n = { lang, t: (key, params) => translate(lang, key, params) };
  return {
    storage,
    render: (component, props) => {
      globalThis.historyHooks.index = 0;
      return component(props);
    },
    reset: () => {
      globalThis.historyHooks = { index: 0, values: [] };
    },
    cleanup: () => {
      if (previousStorage) Object.defineProperty(globalThis, 'localStorage', previousStorage);
      else delete globalThis.localStorage;
      delete globalThis.historyHooks;
      delete globalThis.historyI18n;
    },
  };
}

const start = Date.UTC(2026, 10, 20);
const feed = parseFeed({
  feeds: [
    { created_at: new Date(start).toISOString(), field1: 10 },
    { created_at: new Date(start + 3600000).toISOString(), field1: 12 },
  ],
});
const diag = parseFeed({
  feeds: [
    ...Array.from({ length: 13 }, (_, i) => ({
      created_at: new Date(start + i * 3600000).toISOString(),
      field1: i === 1 ? null : 3840,
      field2: i === 1 ? null : -70,
    })),
    { created_at: new Date(start + 42 * 3600000).toISOString(), field1: 3800, field2: -90 },
    { created_at: new Date(start + 43 * 3600000).toISOString(), field1: 3800, field2: -50 },
  ],
});

test('dashboard view buttons switch actual chart series and independent units in pl/en', () => {
  for (const lang of ['pl', 'en']) {
    const f = fixture(lang);
    try {
      const props = {
        feed,
        diag,
        hasDiag: true,
        range: '7d',
        now: start + 7 * 86400000,
        skipMissing: true,
        onRange: () => {},
        view: 'temperature',
        onView: (view) => {
          props.view = view;
        },
      };
      const render = () => f.render(HistoryCard, props);
      const chart = () => nodes(render()).find((n) => n.type === LineChart);
      const switchView = (view) =>
        nodes(render().props.actions)
          .find((n) => n.type === 'button' && n.props.children === translate(lang, 'dash.historyView.' + view))
          .props.onClick();
      const switchUnits = (unit) =>
        nodes(render())
          .find((n) => n.type === 'button' && n.props.children === unit)
          .props.onClick();

      assert.equal(chart().props.series[0].key, 'p0');
      switchView('battery');
      assert.equal(render().props.title, translate(lang, 'dash.history.battery'));
      assert.equal(chart().props.series.length, 1);
      assert.equal(chart().props.series[0].key, 'battery');
      assert.equal(chart().props.unit, '%');
      assert.deepEqual(chart().props.domain, [0, 100]);
      assert.equal(chart().props.series[0].segments.length, 2);
      assert.deepEqual(chart().props.series[0].segments[0].slice(0, 2), [
        [start, 50],
        [start + 2 * 3600000, 50],
      ]);
      switchUnits('V');
      assert.equal(chart().props.unit, 'V');
      assert.equal(chart().props.digits, 2);
      assert.equal(chart().props.series[0].segments[0][0][1], 3.84);
      switchView('wifi');
      assert.equal(chart().props.series[0].key, 'wifi');
      assert.equal(chart().props.unit, '%');
      assert.deepEqual(
        chart().props.series[0].segments[1].map((p) => p[1]),
        [0, 100],
      );
      switchUnits('dBm');
      assert.equal(chart().props.unit, 'dBm');
      assert.equal(chart().props.series[0].segments[0][0][1], -70);
      switchView('battery');
      assert.equal(chart().props.unit, 'V');
      switchView('temperature');
      assert.equal(chart().props.unit, '°C');
      assert.equal(chart().props.series[0].key, 'p0');
      assert.equal(chart().props.start, props.now - RANGES['7d']);
      assert.equal(chart().props.end, props.now);
    } finally {
      f.cleanup();
    }
  }
});

test('no diagnostic channel disables device views and falls back from a stored device view', () => {
  const f = fixture();
  try {
    const selected = [];
    const tree = f.render(HistoryCard, {
      feed,
      diag: null,
      hasDiag: false,
      view: 'battery',
      onView: (view) => selected.push(view),
      range: '7d',
      now: start + 86400000,
    });
    const buttons = nodes(tree.props.actions).filter((n) => n.type === 'button');
    assert.equal(buttons[0].props.disabled, false);
    assert.equal(buttons[0].props['aria-selected'], true);
    for (const button of buttons.slice(1)) {
      assert.equal(button.props.disabled, true);
      assert.equal(button.props.title, translate('pl', 'dash.historyNeedsDiag'));
      assert.equal(button.props['aria-describedby'], 'history-diag-hint');
      button.props.onClick();
    }
    assert.deepEqual(selected, []);
    assert.ok(nodes(tree).some((n) => n.props?.id === 'history-diag-hint'));
    assert.equal(nodes(tree).find((n) => n.type === LineChart).props.series[0].key, 'p0');
  } finally {
    f.cleanup();
  }
});

test('configured but empty diagnostics keep device views available and never show temperature data', () => {
  const f = fixture();
  try {
    const tree = f.render(HistoryCard, {
      feed,
      diag: null,
      hasDiag: true,
      view: 'wifi',
      onView: () => {},
      range: '7d',
      now: start + 86400000,
    });
    assert.ok(
      nodes(tree.props.actions)
        .filter((n) => n.type === 'button')
        .every((n) => !n.props.disabled),
    );
    assert.deepEqual(nodes(tree).find((n) => n.type === LineChart).props.series[0].segments, []);
  } finally {
    f.cleanup();
  }
});

test('dashboard persists selected view, restores it and preserves ChangesCard', () => {
  const f = fixture('en');
  try {
    f.storage.set('gm.dashboard.v1', JSON.stringify({ channelId: '123', diagChannelId: '456', range: '7d' }));
    f.render(App);
    globalThis.historyHooks.values[3] = { main: feed, diag };
    const tree = f.render(App);
    const history = nodes(tree).find((n) => n.type === HistoryCard);
    assert.equal(history.props.hasDiag, true);
    assert.equal(history.props.view, 'temperature');
    history.props.onView('wifi');
    assert.equal(f.storage.get('gm.dashboard.historyView'), 'wifi');
    assert.equal(nodes(f.render(App)).find((n) => n.type === HistoryCard).props.view, 'wifi');
    f.reset();
    f.render(App);
    globalThis.historyHooks.values[3] = { main: feed, diag };
    const restored = f.render(App);
    assert.equal(nodes(restored).find((n) => n.type === HistoryCard).props.view, 'wifi');
    assert.ok(nodes(restored).some((n) => n.type?.name === 'ChangesCard'));
    const device = nodes(restored).find((n) => n.type?.name === 'DeviceCard');
    assert.ok(!nodes(device.type(device.props)).some((n) => n.type === DeviceCharts));
    f.storage.set('gm.dashboard.historyView', 'invalid');
    f.reset();
    f.render(App);
    globalThis.historyHooks.values[3] = { main: feed, diag };
    assert.equal(nodes(f.render(App)).find((n) => n.type === HistoryCard).props.view, 'temperature');
  } finally {
    f.cleanup();
  }
});

test('portal HistoryCard stays temperature-only with its original missing-value boundaries', () => {
  const f = fixture();
  try {
    const localFeed = parseFeed({
      feeds: [
        { created_at: new Date(start).toISOString(), field1: 10 },
        { created_at: new Date(start + 3600000).toISOString() },
        { created_at: new Date(start + 7200000).toISOString(), field1: 12 },
      ],
    });
    const tree = f.render(HistoryCard, { feed: localFeed, range: '7d', now: start + 86400000, onRange: () => {} });
    assert.ok(!nodes(tree.props.actions).some((n) => n.props?.class === 'seg history-views'));
    assert.equal(nodes(tree).find((n) => n.type === LineChart).props.series[0].segments.length, 2);
  } finally {
    f.cleanup();
  }
});

test('large chart tooltips show the selected units on touch and pointer movement', () => {
  const f = fixture('en');
  try {
    for (const [unit, digits, value, expected] of [
      ['%', 0, 50, '50 %'],
      ['V', 2, 3.84, '3.84 V'],
      ['dBm', 0, -70, '-70 dBm'],
    ]) {
      f.reset();
      const props = {
        lang: 'en',
        start,
        end: start + 3600000,
        unit,
        digits,
        series: [{ key: 'device', label: 'Device', color: 'blue', segments: [[[start, value]]] }],
      };
      const render = () => f.render(LineChart, props);
      const svg = nodes(render()).find((n) => n.type === 'svg');
      svg.ref.current = { getBoundingClientRect: () => ({ left: 0, width: 800 }) };
      svg.props.onPointerDown({ clientX: 46, pointerType: 'touch' });
      assert.ok(nodes(render()).some((n) => n.type === 'b' && n.props.children === expected));
      svg.props.onPointerLeave();
      assert.ok(!nodes(render()).some((n) => n.props?.class?.startsWith('chart-tip')));
      svg.props.onPointerMove({ clientX: 46, pointerType: 'mouse' });
      assert.ok(nodes(render()).some((n) => n.type === 'b' && n.props.children === expected));
    }
  } finally {
    f.cleanup();
  }
});
