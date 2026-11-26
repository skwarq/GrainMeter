import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { parseFeed } from '../src/lib/feed.js';
import { translate } from '../src/i18n/translate.js';

const bundle = await build({
  stdin: {
    contents: "export { ChangesCard } from './src/dashboard/ChangesCard.jsx';",
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
      name: 'changes-view-fixtures',
      setup(b) {
        b.onResolve({ filter: /^preact\/hooks$/ }, () => ({ path: 'hooks', namespace: 'fixtures' }));
        b.onLoad({ filter: /^hooks$/, namespace: 'fixtures' }, () => ({
          contents: `export const useState = (v) => [globalThis.changesType ?? v, (n) => { globalThis.changesType = n; }];
          export const useMemo = (fn) => fn();
          export const useContext = () => globalThis.changesI18n;
          export const useEffect = () => {};
          export const useRef = (v) => ({current: v});`,
        }));
      },
    },
  ],
});
const { ChangesCard } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);

function nodes(tree) {
  if (!tree || typeof tree !== 'object') return [];
  if (Array.isArray(tree)) return tree.flatMap(nodes);
  return [tree, ...nodes(tree.props?.children)];
}

test('changes card renders bilingual limited rows, direction, safe labels and a working type filter', () => {
  const main = parseFeed({
    channel: { field1: '<img src=x onerror=alert(1)>' },
    feeds: Array.from({ length: 205 }, (_, i) => ({
      created_at: new Date(Date.UTC(2026, 10, 20) + i * 3600000).toISOString(),
      field1: i % 2 ? 10 : 11,
    })),
  });
  try {
    for (const lang of ['pl', 'en']) {
      const t = (key, params) => translate(lang, key, params);
      globalThis.changesI18n = { lang, t };
      delete globalThis.changesType;
      const tree = ChangesCard({ main, diag: null });
      assert.equal(tree.type, 'details');
      assert.ok(!tree.props.open);
      const elements = nodes(tree);
      assert.equal(elements.find((n) => n.type === 'summary').props.children, t('changes.title'));
      assert.equal(elements.find((n) => n.type === 'tbody').props.children.length, 200);
      assert.equal(
        elements.find((n) => n.props?.role === 'status').props.children,
        t('changes.count', { n: 200, total: 204 }),
      );
      assert.ok(elements.some((n) => n.props?.class === 'changes-rise'));
      assert.ok(elements.some((n) => n.props?.class === 'changes-fall'));
      assert.ok(elements.some((n) => n.type === 'td' && n.props.children === main.fields[0].label));
      assert.ok(!elements.some((n) => n.props?.dangerouslySetInnerHTML));
      assert.ok(elements.some((n) => n.props?.class === 'table-wrap' && n.props.tabIndex === 0));
      elements.find((n) => n.type === 'select').props.onChange({ currentTarget: { value: 'device' } });
      const filtered = nodes(ChangesCard({ main, diag: null }));
      assert.equal(
        filtered.find((n) => n.props?.role === 'status').props.children,
        t('changes.count', { n: 0, total: 0 }),
      );
      assert.ok(filtered.some((n) => n.props?.children === t('changes.empty')));
      assert.ok(!filtered.some((n) => n.type === 'table'));
    }
  } finally {
    delete globalThis.changesType;
    delete globalThis.changesI18n;
  }
});
