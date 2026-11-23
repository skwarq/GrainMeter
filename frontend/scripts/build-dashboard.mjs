// Remote dashboard (GitHub Pages): ../build/dashboard/{index.html,
// manifest.webmanifest, icon.svg}. Single page with a strict Content
// Security Policy: the inline script and style are allowed by their SHA-256
// hash only, the network only to the ThingSpeak API.
//
//   GM_API_BASE=http://127.0.0.1:8099 node scripts/build-dashboard.mjs
// builds a test variant that reads from a local stand-in instead.
import * as esbuild from 'esbuild';
import { createHash } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { ROOT, escapeInline, esbuildOptions } from './bundle.mjs';

const API_BASE = process.env.GM_API_BASE || 'https://api.thingspeak.com';
const OUT_DIR = process.env.GM_DASHBOARD_OUT || path.resolve(ROOT, '..', 'build', 'dashboard');

const result = await esbuild.build(
  esbuildOptions({
    dev: false,
    entry: 'src/dashboard/main.jsx',
    define: { __GM_API_BASE__: JSON.stringify(API_BASE) },
  }),
);
const js = result.outputFiles.find((f) => f.path.endsWith('.js'));
const css = result.outputFiles.find((f) => f.path.endsWith('.css'));
const scriptText = escapeInline(js.text.trim(), 'script');
const styleText = css ? escapeInline(css.text.trim(), 'style') : '';
const sha = (s) => "'sha256-" + createHash('sha256').update(s, 'utf8').digest('base64') + "'";

const csp = [
  "default-src 'none'",
  `script-src ${sha(scriptText)}`,
  `style-src ${styleText ? sha(styleText) : "'none'"}`,
  `connect-src ${new URL(API_BASE).origin}`,
  "img-src 'self' data:",
  "manifest-src 'self'",
  "base-uri 'none'",
  "form-action 'none'",
].join('; ');

const template = await readFile(path.join(ROOT, 'src/dashboard/index.html'), 'utf8');
const html = template
  .replace('<!--CSP-->', () => `<meta http-equiv="Content-Security-Policy" content="${csp}" />`)
  .replace('<!--STYLE-->', () => (styleText ? `<style>${styleText}</style>` : ''))
  .replace('<!--SCRIPT-->', () => `<script>${scriptText}</script>`);

const icon = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">
<rect x="1" y="1" width="30" height="30" rx="8" fill="#a8620a"/>
<path d="M8 13 16 6l8 7v12H8z" fill="none" stroke="#fffaf0" stroke-width="2" stroke-linejoin="round"/>
<path d="M9 19c2.5-1.6 4.5-1.6 7 0s4.5 1.6 7 0v6H9z" fill="#fffaf0" opacity=".45"/>
<path d="M16 9v10" stroke="#fffaf0" stroke-width="1.6" stroke-linecap="round"/>
<circle cx="16" cy="20.5" r="2.2" fill="#fffaf0"/>
</svg>
`;
const manifest = {
  name: 'GrainMeter',
  short_name: 'GrainMeter',
  description: 'Temperatura ziarna w silosie / grain temperature in the silo',
  start_url: '.',
  scope: '.',
  display: 'standalone',
  background_color: '#f3eee3',
  theme_color: '#a8620a',
  icons: [{ src: 'icon.svg', sizes: 'any', type: 'image/svg+xml', purpose: 'any' }],
};

await mkdir(OUT_DIR, { recursive: true });
await writeFile(path.join(OUT_DIR, 'index.html'), html);
await writeFile(path.join(OUT_DIR, 'icon.svg'), icon);
await writeFile(path.join(OUT_DIR, 'manifest.webmanifest'), JSON.stringify(manifest, null, 2) + '\n');
console.log(
  `dashboard: ${path.relative(process.cwd(), OUT_DIR)}/index.html ${(html.length / 1024).toFixed(1)} KiB, API ${API_BASE}`,
);
