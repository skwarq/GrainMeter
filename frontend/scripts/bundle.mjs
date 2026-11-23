// Shared by build.mjs and dev.mjs: bundles src/main.jsx with esbuild and
// inlines the JS and CSS into src/index.html -> one self-contained page.
import * as esbuild from 'esbuild';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

export function esbuildOptions({ dev = false } = {}) {
  return {
    absWorkingDir: ROOT,
    entryPoints: ['src/main.jsx'],
    bundle: true,
    write: false,
    outdir: 'out',
    format: 'iife',
    platform: 'browser',
    // phones in the field may run older WebViews
    target: ['es2020', 'chrome87', 'safari15', 'firefox90'],
    jsx: 'automatic',
    jsxImportSource: 'preact',
    minify: !dev,
    sourcemap: dev ? 'inline' : false,
    legalComments: 'none',
    charset: 'utf8',
    define: { 'process.env.NODE_ENV': dev ? '"development"' : '"production"' },
    logLevel: 'warning',
  };
}

/** Makes inline <script>/<style> content safe against premature end tags. */
export function escapeInline(code, tag) {
  return code.replace(new RegExp('</(' + tag + ')', 'gi'), '<\\/$1').replace(/<!--/g, '<\\!--');
}

export async function inlineHtml(outputFiles, { extraHead = '' } = {}) {
  const js = outputFiles.find((f) => f.path.endsWith('.js'));
  const css = outputFiles.find((f) => f.path.endsWith('.css'));
  if (!js) throw new Error('esbuild produced no JavaScript');
  const template = await readFile(path.join(ROOT, 'src/index.html'), 'utf8');
  const style = css ? `<style>${escapeInline(css.text.trim(), 'style')}</style>` : '';
  const script = `<script>${escapeInline(js.text.trim(), 'script')}</script>`;
  // function replacers: "$" sequences in the bundle must not be interpreted
  return template.replace('<!--STYLE-->', () => style + extraHead).replace('<!--SCRIPT-->', () => script);
}

export async function bundleOnce(opts) {
  const result = await esbuild.build(esbuildOptions(opts));
  return inlineHtml(result.outputFiles);
}
