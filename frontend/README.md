# GrainMeter service portal (frontend)

Single-page web UI served by the device in service mode (SoftAP + captive
portal) and by the PC simulator. Preact + esbuild; the build produces **one
self-contained file** (all JS, CSS and SVG inlined, no fonts, no CDN, no
external requests) because the phone connected to the device AP has no internet.

- Runtime dependency: `preact` only. Dev: `esbuild`, `eslint`, `@eslint/js`, `globals`, `prettier`.
- Versions are pinned exactly in `package.json`; `package-lock.json` is committed.
- Node: see `.nvmrc` (current LTS, 26).

## Output

`npm run build` writes into `../portal/`:

| File                   | Use                                                 |
| ---------------------- | --------------------------------------------------- |
| `portal/index.html`    | served by the PC simulator (`--portal-dir`)         |
| `portal/index.html.gz` | embedded in the firmware (`Content-Encoding: gzip`) |

The `.gz` is deterministic (zlib level 9 from Node, header mtime 0, no file
name), so identical sources give identical bytes. **Size budget: gz ≤ 60 KiB**
— the build fails above it (currently ~38 KiB).

## Commands

Local Node may be too old; everything runs fine in Docker:

```sh
cd frontend
alias gnode='docker run --rm -it -u $(id -u):$(id -g) -e HOME=/tmp --network host \
  -v "$PWD/..":/g -w /g/frontend node:26-alpine'

gnode npm ci            # install exactly the locked versions
gnode npm run verify    # prettier --check, eslint, unit tests, build
gnode npm run build     # only the build (-> ../portal/)
gnode npm test          # node:test unit tests (test/*.test.js)
gnode npm run format    # prettier --write
```

(`/g` mounts the whole repo because the build writes to `../portal`.)

## Development against the simulator

```sh
# terminal 1: simulator (from the repo root)
build/dev/pc/grainmeter_pc --port 8080 --speed 120 --password devpass123 \
  --data-dir /tmp/gm-sim --no-console

# terminal 2: dev server with live reload, /api/* proxied to the simulator
cd frontend
gnode npm run dev                     # http://127.0.0.1:5173/
# other target / port:
#   GM_API=http://192.168.4.1 PORT=5173 HOST=0.0.0.0 npm run dev
```

Alternatively build and let the simulator serve the real output directly:
`npm run build` and open `http://127.0.0.1:8080/` (the simulator serves `portal/`).

For OTA tests, `npm run fake-ota -- /tmp/fake.bin 0.2.0-test [project] [chipId]`
writes a fake image header (magic 0xE9, chip 0x0D = ESP32-C6, app descriptor
with version and project). A wrong project or chip id must be rejected with 415.

## Structure

```
src/
  api.js            the ONLY place using fetch/XMLHttpRequest (timeouts, auth, OTA upload)
  app.jsx           shell: hash routing, status polling (4 s, paused in hidden tabs), unlock dialog
  pages/            Overview, Sensors, Config, Upload, Update, Diagnostics, Simulation, Service
  components/       UI primitives, silo SVG, hooks, i18n context, unlock dialog
  lib/              pure helpers (unit tested): temperature scale, silo geometry,
                    battery %, error mapping, formatting, OTA header pre-check
  i18n/pl.js, en.js message tables (Polish default, English switch, same keys)
  index.html        page template; styles.css: own design, light + dark (prefers-color-scheme)
scripts/            build.mjs, dev.mjs, bundle.mjs (shared inliner), fake-ota.mjs
test/               node:test unit tests
```

## Rules

- Device strings are rendered only as Preact text nodes; ESLint forbids
  `innerHTML`/`dangerouslySetInnerHTML` and `fetch`/`XMLHttpRequest` outside `src/api.js`.
- Missing temperatures are `null` and are never shown or coloured as 0 °C.
- The service password is asked on the first write action, checked with
  `POST /api/login`, kept in `sessionStorage` and sent as `X-GM-Password`;
  after a 401 it is cleared and asked again. Secrets are write-only in the
  configuration form (empty field = keep).
- No analytics, no secrets in the repository.

## Silo drawing

Markers are drawn to scale from `height_cm` (0 = probe tip) when every mapped
point has a height; otherwise all points are evenly spaced with point 1 at the
bottom. Error points are hatched red with "!", points without a sensor are
dashed outlines.
