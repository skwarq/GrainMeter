import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { build } from 'esbuild';

const source = await readFile(new URL('../src/remote/main.jsx', import.meta.url), 'utf8');
const bundle = await build({
  stdin: {
    loader: 'jsx',
    contents:
      source.replace('function Remote()', 'export function Remote()').replace(/render\(<Remote[\s\S]*$/, '') +
      "\nexport { configure } from 'preact/hooks';",
    resolveDir: new URL('../src/remote/', import.meta.url).pathname,
  },
  bundle: true,
  write: false,
  format: 'esm',
  platform: 'node',
  loader: { '.css': 'empty' },
  jsx: 'automatic',
  jsxImportSource: 'preact',
  plugins: [
    {
      name: 'connection-fixtures',
      setup(b) {
        b.onResolve({ filter: /^preact\/hooks$/ }, () => ({ path: 'hooks', namespace: 'fixtures' }));
        b.onLoad({ filter: /^hooks$/, namespace: 'fixtures' }, () => ({
          contents: `let values, index;
          export function configure(v) { values = v; index = 0; }
          export const useEffect = () => {};
          export const useContext = () => {};
          export const useState = () => [values[index++], () => {}];`,
        }));
        b.onResolve({ filter: /^\.\.\/app\.jsx$/ }, () => ({ path: 'app', namespace: 'fixtures' }));
        b.onLoad({ filter: /^app$/, namespace: 'fixtures' }, () => ({
          contents: 'export function App() { return null; }',
        }));
      },
    },
  ],
});
const fixture = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);

function nodes(tree) {
  if (!tree || typeof tree !== 'object') return [];
  if (Array.isArray(tree)) return tree.flatMap(nodes);
  return [tree, ...nodes(tree.props?.children)];
}

test('remote connection shows bilingual derivation state and hides unsigned session deadlines', () => {
  for (const lang of ['pl', 'en']) {
    for (const status of [
      { state: 'deriving', verified: false },
      { state: 'online', verified: false, until: 1800000000 },
      { state: 'online', verified: true, until: 1800000000 },
    ]) {
      fixture.configure([
        { form: {}, remembered: false },
        {},
        false,
        lang,
        null,
        status,
        null,
        false,
        '',
        false,
        false,
        true,
      ]);
      const elements = nodes(fixture.Remote());
      const text = String(elements.find((n) => n.props?.role === 'status').props.children);
      assert.ok(
        text.startsWith(
          status.state === 'deriving'
            ? lang === 'pl'
              ? 'Wyliczanie klucza...'
              : 'Deriving key...'
            : lang === 'pl'
              ? 'Urządzenie online'
              : 'Device online',
        ),
        text,
      );
      assert.equal(text.includes(' · '), status.verified);
      const bar = elements.find((n) => n.type?.name === 'SessionBar');
      assert.equal(bar.props.online, status.verified);
    }
  }
});
