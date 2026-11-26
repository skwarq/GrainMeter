// Production build: ../portal/index.html (single file) + index.html.gz
// (deterministic: zlib level 9, no file name, mtime 0).
import { mkdir, writeFile } from 'node:fs/promises';
import { gzipSync, constants } from 'node:zlib';
import path from 'node:path';
import { ROOT, bundleOnce } from './bundle.mjs';

const OUT_DIR = path.resolve(ROOT, '..', 'portal');
const GZ_BUDGET = 72 * 1024; // served from flash (EMBED_FILES), not RAM

const html = await bundleOnce({ dev: false });
if (/\s(src|href)=["']?(https?:)?\/\//i.test(html)) {
  throw new Error('external reference found in the page; everything must be inlined');
}
const raw = Buffer.from(html, 'utf8');
// Node's gzip header has MTIME = 0 and no file name, so the output only
// depends on the input bytes and the zlib version.
const gz = gzipSync(raw, { level: constants.Z_BEST_COMPRESSION, memLevel: 9 });

await mkdir(OUT_DIR, { recursive: true });
await writeFile(path.join(OUT_DIR, 'index.html'), raw);
await writeFile(path.join(OUT_DIR, 'index.html.gz'), gz);

const kib = (n) => (n / 1024).toFixed(1) + ' KiB';
console.log(`portal/index.html     ${raw.length} B (${kib(raw.length)})`);
console.log(`portal/index.html.gz  ${gz.length} B (${kib(gz.length)}), budget ${kib(GZ_BUDGET)}`);
if (gz.length > GZ_BUDGET) {
  console.error(`size budget exceeded by ${gz.length - GZ_BUDGET} B`);
  process.exit(1);
}
