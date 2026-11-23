// Development server: rebuilds the single-file page on every change, serves
// it on http://localhost:5173 and proxies /api/* to the device or simulator.
//   GM_API=http://127.0.0.1:8080 npm run dev     (default target)
//   PORT=5173 HOST=0.0.0.0 npm run dev
import * as esbuild from 'esbuild';
import http from 'node:http';
import { ROOT, esbuildOptions, inlineHtml } from './bundle.mjs';

const target = new URL(process.env.GM_API || 'http://127.0.0.1:8080');
const port = Number(process.env.PORT || 5173);
const host = process.env.HOST || '127.0.0.1';

let page = '<!doctype html><p>building…</p>';
const clients = new Set();
const RELOAD = `<script>new EventSource('/__reload').onmessage=()=>location.reload()</script>`;

const ctx = await esbuild.context({
  ...esbuildOptions({ dev: true }),
  plugins: [
    {
      name: 'inline-html',
      setup(build) {
        build.onEnd(async (res) => {
          if (res.errors.length) return;
          page = await inlineHtml(res.outputFiles, { extraHead: RELOAD });
          for (const c of clients) c.write('data: reload\n\n');
          console.log(`[dev] rebuilt ${new Date().toLocaleTimeString()}`);
        });
      },
    },
  ],
});
await ctx.watch();

http
  .createServer((req, res) => {
    if (req.url.startsWith('/api/')) {
      const up = http.request(
        {
          host: target.hostname,
          port: target.port || 80,
          path: req.url,
          method: req.method,
          headers: { ...req.headers, host: target.host },
        },
        (r) => {
          res.writeHead(r.statusCode, r.headers);
          r.pipe(res);
        },
      );
      up.on('error', (e) => {
        if (!res.headersSent) res.writeHead(502, { 'content-type': 'application/json' });
        res.end(JSON.stringify({ ok: false, error: 'dev proxy: ' + e.message }));
      });
      req.pipe(up);
      return;
    }
    if (req.url === '/__reload') {
      res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' });
      res.write(': hi\n\n');
      clients.add(res);
      req.on('close', () => clients.delete(res));
      return;
    }
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
    res.end(page);
  })
  .listen(port, host, () => {
    console.log(`[dev] http://${host}:${port}/  (api -> ${target.origin}, root ${ROOT})`);
  });
