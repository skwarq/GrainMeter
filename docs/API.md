# GrainMeter portal REST API

The same API is served by the PC simulator (cpp-httplib) and by the ESP32
firmware in service mode (esp_http_server). Request routing and JSON are
produced by shared code (`core/app/portal.cpp`, `core/app/grainmeter.cpp`), so
both platforms answer identically. Sample responses: `docs/api-samples/`.

## Conventions

- Responses are `application/json`. Errors: `{"ok":false,"error":"text"}` with
  a 4xx/5xx status. Simple actions: `{"ok":true,"message":"text"}`.
- Temperatures are numbers in °C with two decimals, or `null` when there is no
  valid reading. A missing value is **never** `0`; `0.00` is a real reading.
- Every `POST` requires the service password in the `X-GM-Password` header.
  Wrong/missing password: `401`. After 5 wrong attempts writes are locked for
  60 s (also `401`). A custom header cannot be sent cross-site without a CORS
  preflight, which the device never allows (CSRF protection).
- Form bodies are `application/x-www-form-urlencoded`, max 2 KiB.
- Secrets (Wi-Fi password, write keys, service password) are never returned;
  the API reports `*_set: true/false` instead.

## Read-only endpoints

| Method | Path | Result |
| --- | --- | --- |
| GET | `/api/status` | compact status for the dashboard (poll every few seconds) |
| GET | `/api/diag` | full diagnostics: status + `config`, `rom_map`, `scan` |
| GET | `/api/logs/<since>` | live log preview; bounded JSON page, no query string |
| GET | `/api/config` | configuration without secrets |
| GET | `/api/ota` | state of the current/last firmware upload |
| GET | `/api/history` | local history summary (JSON, below) |
| GET | `/api/history/<from>/<to>` | local history page (binary, below) |

### `/api/logs/<since>`

`since` is the last delivered unsigned decimal line sequence, initially `0`.
Response: `{"next":42,"lost":3,"lines":["I (123) app: ready"]}`.
Poll from `next` every approximately 2 seconds. `next` is the last line
delivered, not necessarily the newest buffered line (continue paging).
Evicted/expired lines after `since` are counted in `lost`; delivery starts
at the oldest retained line. A future cursor returns no lines and resets
`next` to the current last sequence, allowing recovery after reboot.
Malformed/overflowing cursors return `400`; unavailable storage returns `503`.
JSON responses are below **8 KiB**, including escaping; complete lines are
truncated to 255 bytes. Treat lines as untrusted text, never HTML.

Like `/api/diag`, this read-only endpoint requires **no service password
locally**; the MQTT tunnel still authenticates every request with HMAC and
the service password. Logs must not contain secrets. No UDP/MQTT logging
configuration is needed. ESP32 lazily allocates approximately 4.3 KiB on
the first read, capturing only subsequent logs, and frees it after 5 minutes
without reads (sender task checks every 250 ms). Sequence survives this
expiry, but not reboot; lines emitted while inactive are not counted.
The hook never allocates or waits for the preview lock; contention can
drop fragments (not included in `lost`, which counts retained-line eviction).
The PC simulator supplies synthetic `I (...) pc:` lines on each read.

The Diagnostics Logs panel starts polling when opened; closing it, leaving
the page or pausing stops polling. It keeps 500 lines, filters E/W/I/D,
reports loss, clears the local view without rewinding `next`, and downloads
filtered plain UTF-8 text. `log_level` controls remote-enabled verbosity;
with remote forwarding disabled the existing firmware level remains Info.

### `/api/status` fields

`firmware`, `platform` (`"pc-sim"` or `"esp32c6"`), `name`, `mono_s`,
`time_valid`, `epoch` (UTC, 0 if unknown), `wake_reason`, `reset_reason`,
`battery_mv` (number or `null`), `wifi{state,ssid,rssi}`, `config_ok`,
`upload_configured`, `schedule{measure_s,upload_s,samples_per_upload,
next_measure_in_s,upload_due}`, `measurement` (or `null`), `buffer`,
`last_upload` (or `null`), `power`, `remote_update`, `remote`, `errors[]`, `ota`, and
a platform object: `sim` on PC, `esp` on the ESP32.

`clock` (only in `/api/diag`): `source` (`none`, `sntp`, `http_date`),
`sync_age_s` (or `null`),
`last_fix_s`, and `restored` (boolean). `restored` means this boot accepted
a checked RTC clock snapshot after software/panic/watchdog reset; it stays
true until the next boot, even after network synchronization. Recovery
does not count as a synchronization: `source` is initially `none` and the
next upload window retries SNTP. Native deep-sleep time and the PC simulator
report `restored: false`. Remote-session startup also attempts bounded SNTP
when the station is connected and the clock is unknown, before MQTT starts;
failure is logged and `until` remains zero if time is still unavailable.

`battery_runtime`: `{"mah_per_day":3.50,"capacity_mah":2500,"soc_pct":50,
"days":357.14}`. Consumption and days are numbers with two decimal places,
or `null` when unknown. `soc_pct` is integer 0..100 or `null` without a
valid voltage, using the same interpolated light-load LiPo curve as the UI.
Days = capacity × SoC / 100 / consumption (zero consumption gives `null`;
empty battery gives 0 days). `mah_per_day` comes from measured awake,
boot, Wi-Fi and completed sleep times with typical currents, not a current
sensor; it is unavailable until at least one hour has elapsed. ESP diagnostics
`esp.power_stats.est_mah_per_day` uses the same value and unknown convention.
The PC simulator defaults to unknown consumption. This estimate ignores solar
charging, self-discharge, temperature and ageing; the independent voltage
trend forecast (`battery_forecast`) is unchanged.

`power`: `mode` (`normal` / `economy` / `critical`), `battery_mv` (or
`null`), `low_mv`, `crit_mv` (0 = threshold disabled). Economy: uploads 4x
less often. Critical: no radio at all, measuring interval doubled.

`remote` (also in `/api/diag`): `configured` (nonempty MQTT URL),
`talkback` (nonzero TalkBack id), `budget_min`, `budget_left_min` (whole
minutes rounded down), `pending_min` (after expiry, budget and Critical checks),
`last_cmd{text,epoch,mono_s}` (last consumed command, credentials redacted;
epoch 0 when unknown, empty text before any command). Pending requests and
last command diagnostics live in RAM; usage is retained across deep sleep.

`remote_update`: `enabled` (periodic checks), `url_set` (manual check
possible), `running` (version), `latest` (from the manifest, "" if never
seen), `result` (`never_checked`, `up_to_date`, `available`, `updated`,
`failed`, `skipped`, `service_requested`), `message`,
`rejected` (version rolled back after a failed first start; never retried),
`last_check_age_s` (or `null`).

`measurement`: `mono_s`, `age_s`, `epoch`, `presence`, `conversion_ms`,
`conversion_timeout`, `points[8]` (`point`, `rom`|null, `height_cm`|null,
`status`, `temp_c`|null) and `unmapped[]` (`rom`, `family`, `family_name`,
`status`, `temp_c`).

Sensor `status` values: `ok`, `missing`, `crc_error`, `unsupported_family`,
`power_on_value` (85 °C register value, conversion did not run), `bus_error`
(line stuck low), `out_of_range`, `not_mapped`.

`buffer`: `count`, `capacity`, `next_seq`, `oldest_seq`, `dropped_overflow`,
`dropped_no_time`, `upload_ok`, `upload_fail`, `wake_count`,
`recovered_after_power_loss`.

`last_upload`: `ok`, `diag_ok`, `http_status`, `epoch`, `age_s`,
`samples_sent`, `samples_confirmed`, `message`.

`errors[]`: up to 8 most recent `{age_s, text}`, oldest first.

`ota`: `state` (`idle`/`receiving`/`done`/`failed`), `received`, `expected`,
`file`, `error`, and after a valid header `version`, `project`, `idf`.

`esp` (ESP32 only): `service_mode`, `ap_ssid`, `ap_ip`, `ap_clients`,
`service_timeout_s`, `running_partition`, `next_partition`, `slot_size`,
`app_state` (`valid`/`pending_verify`/...), `free_heap`, `onewire_gpio`,
`service_wake_gpio` (-1 = none), `rejected_firmware`.

ESP32 diagnostics also include `esp.logs`:
`{"log_dest":"udp","mqtt_sent":0,"mqtt_dropped":0}`. MQTT counters count
complete lines (not messages), since boot, excluding the synthetic dropped-line
notice. `mqtt_sent` means accepted by the local QoS 0 client, not acknowledged
by the broker. Drops include oldest-line eviction and failed publication.
The existing `esp.udp_log{sent,dropped}` counters remain UDP-only.

`sim` (PC only): `speed`, `network_up`, `lost_response`, `http_posts`,
`mock_entries_main`, `mock_entries_diag`, `config_file`,
`ota_slots{running,pending_new_image,last_boot,rejected,fail_next_boot}`,
`battery_override_mv` (0 = simulated curve), `service_requests`,
`time_syncs`, `http_sessions`, `talkback_calls`,
`probe{shorted, devices[{index, rom, present, crc_faults, temp_c}]}`.

### `/api/history` (JSON)

```json
{"available":true,"records":1077,"used_bytes":11626,"capacity_bytes":192512,
 "oldest_epoch":1795000000,"newest_epoch":1797591600,
 "last_measure_epoch":1797592500,"heartbeat_s":10800,"deadband_centi":15,
 "max_gap_s":12600}
```

`available:false` (and `503` on the pages) when the device has no `history`
partition. `/api/diag` contains the same object under `history`.

### `/api/history/<from>/<to>` (binary)

`from`, `to`: UTC epoch seconds, inclusive. The portal asks per local day.
`application/octet-stream`, little endian.
Over the MQTT tunnel the same bytes are sent as standard padded base64 in
`body`, with `"enc":"base64"`; the response MAC covers the base64 text.
Both HTTP and MQTT browser transports return an `ArrayBuffer`.

Header (16 B): `"GMH3"`, `uint16 rows`, `uint16 points` (8), `uint32 next`
(0 = complete; otherwise more than 256 rows: request again with
`from = next`), `uint32 max_gap` (s).

Rows (24 B):

| offset | type | meaning |
|---|---|---|
| 0 | `uint32` | epoch (UTC s) |
| 4 | `int16[8]` | °C x 100 per point, `-32768` = no value |
| 20 | `uint16` | battery mV (10 mV resolution), `0` = unknown |
| 22 | `int8` | Wi-Fi RSSI dBm of the last connection, `0` = unknown |
| 23 | `uint8` | flags: bits 0-1 power mode (0 normal, 1 economy, 2 critical, 3 unknown), bits 2-3 last upload (0 none yet, 1 ok, 2 failed), bit 4 time reconstructed, bit 5 first measurement after power-up, bit 6 held state until the last measurement |

Rows are the stored changes, not every measurement: draw straight lines
between them (the device stores the last unchanged measurement before a
change, so lines stay within the 0.15 °C dead band). Rows further apart than
`max_gap` (heartbeat 3 h + 2 measurement intervals) mean no data in between;
bit 5 also starts a new line. The first row may be the last change before
`from` (state at the range start) and the last row the held state at the last
measurement (bit 6), so consecutive pages/days overlap by one row:
deduplicate by time.

## Actions (POST, password required)

| Path | Body | Effect |
| --- | --- | --- |
| `/api/login` | – | checks the password (`200` or `401`) |
| `/api/config` | any of `name, wifi_ssid, wifi_pass, wifi_pass_clear, report_url, ts_channel, ts_key, diag_channel, diag_key, measure_s, upload_s, svc_pass, diag_every_h, batt_low_mv, batt_crit_mv, batt_capacity_mah, update_url, update_check_h, static_ip, gateway, netmask, dns` | validates all, saves (NVS / file); empty secret = keep |
| `/api/scan` | – | Search ROM; returns `/api/diag` |
| `/api/measure` | `store` (optional) | read all sensors; returns `/api/status` |
| `/api/upload` | – | one upload window now: `{ok,message,http_status,samples_confirmed}` |
| `/api/update/check` | – | connect Wi-Fi, read the update manifest now (no download): `{ok,result,message,restarting:false}`; `result` `available` when newer |
| `/api/update/install` | `version` (optional) | as check, then download, verify and install a newer version: `{ok,result,message,restarting}`; the device restarts after the reply when `restarting`. With `version`, only that version is installed (a changed manifest gives `available` + "check again"). Manual checks do not move the periodic schedule, do not consume a `service_id` request and are refused in critical power mode. |
| `/api/map` | `rom`, `point` (1..8), `height_cm` (optional, -1..32767) | assign sensor to point |
| `/api/unmap` | `rom` | remove assignment |
| `/api/automap` | – | assign unmapped sensors to free points (ROM order) |
| `/api/restart` | – | reply, then restart |
| `/api/service/exit` | – | ESP32: leave service mode, resume deep-sleep cycle |
| `/api/sim` | `action` + args | PC only, fault injection (below) |
| `/api/ota` | firmware file | see below |
| `/api/ota/begin` | `size` (bytes), `sha256` (64 hex), `name` | start chunked upload: `{ok:true,chunk:1440,next:0}` |
| `/api/ota/chunk` | `off` (decimal byte offset), `d` (base64url) | `{ok:true,next}`; at most 1440 decoded bytes |
| `/api/ota/finish` | `sha256` (64 hex, same as begin) | verify, select boot slot and restart after reply: `{ok:true,version,bytes,restarting:true}` |
| `/api/ota/abort` | – | cancel chunked upload: `{ok:true}`; does not cancel another HTTP upload |

### Firmware upload `POST /api/ota`

Either a normal HTML file form (`multipart/form-data`, first file part is
used; any file name) or the raw image (`application/octet-stream`, optional
`X-File-Name`). The content decides: ESP-IDF image magic, chip ESP32-C6,
project `grainmeter`, size within the OTA slot. The image is streamed to the
inactive slot and selected for boot only after full validation; then the
reply `{ok:true, version, bytes}` is sent and the device restarts. Errors:
`401`, `409` (busy), `411`, `413` (too large), `415` (not a GrainMeter
ESP32-C6 image), `422` (image verification failed), `500` (flash error).

### Chunked firmware upload (HTTP and MQTT tunnel)

The four `/api/ota/*` POST paths above require the same service password as
the streaming endpoint. Bodies are form-urlencoded, limited to 2048 bytes.
Encode `d` as base64url without padding: 1440 bytes become 1920 characters,
leaving room for `off`. The decoder also accepts standard base64 and trailing
padding; encode `+` as `%2B` when using the standard alphabet.

`begin` reserves the **same** `OtaSession` used by HTTP and manifest updates.
No flash is erased or written until `chunk` supplies a validated image header.
Consequently the existing target's potentially slow erase occurs in the first
header-completing `chunk`, not in `begin` (R3 must measure that latency on ESP32).
SHA-256 is accumulated only for newly accepted bytes. `finish` must match both
the digest from `begin` and the complete received image, before image validation
and boot-slot selection. `GET /api/ota` returns the existing state fields plus
`received`, `expected`, and `chunk:1440` (also when idle).

Send at `off == received`; a fully received duplicate (`off + len <= received`)
returns 200 without writing or hashing again. A gap or partial overlap returns
`409 {"ok":false,"error":"offset","next":<received>}`. Resume in the same session
from `GET /api/ota`. No accepted chunk (including retries) for **120 seconds**
aborts the upload; status polling does not extend this deadline.

Errors: `400` (invalid fields, base64, empty/oversized chunk or incomplete image),
`401` (password), `411` (zero size), `409 busy` (another OTA), `409 no_upload` (no active chunked
upload), `413` (slot/body size), `415` (image header), `422 sha256` (digest
mismatch, aborts), `422` (target verification), `500` (flash), and
`503 low_battery` (begin in Critical mode). Image/flash errors abort the upload.
Use a **60-second** request timeout for `begin` and `finish`.

Firmware integration (R3): `app.otaUpload().setCanBegin(callback, context)`
can reject with `409 session_short` and extend the remote session on acceptance.
The core does not manage remote-session deadlines or energy budgets. Call
`otaUpload().abort()` when the remote session ends, and `otaUpload().tick()`
from platform loops that skip `runDue()` while OTA is busy. `runDue()`, portal
requests and the PC loop already check the monotonic 120-second timeout.

### Simulation `POST /api/sim` (PC)

`action=` `crc` (`sensor`, `count` = reads, -1 = permanent), `remove` /
`restore` (`sensor`), `temp` (`sensor`, `value` °C), `unsupported`
(`arg=on|off`, adds a DS18S20 0x10), `short` (`arg=on|off`, DATA stuck low),
`net` (`arg=down|up`), `lost_response` (`arg=on|off`: server stores, reply
lost), `speed` (`value`), `advance` (`seconds`), `powerloss`, `battery`
(`value` mV, 0 = simulated curve), `ota_fail_boot` (`arg=on|off`: the next
new image fails its first start and is rolled back).

`action=talkback&cmd=REMOTE+5+0` appends one command (max 127 bytes) to the
mock TalkBack FIFO. When `report_url` uses `mock://`, TalkBack uses
`mock://talkback/talkbacks/<id>/commands/execute.json`; otherwise it uses
`https://api.thingspeak.com`. No polling when `talkback_id=0` or Critical.

## Configuration fields (new in v0.2)

| Field | Range | Meaning |
| --- | --- | --- |
| `remote_url` | empty or `mqtts://host[:port]`, max 96 | empty = MQTT tunnel off; port 1..65535 |
| `remote_user` | max 64 printable characters | device broker login |
| `remote_pass` | max 64 printable characters, secret | JSON: `remote_pass_set`; empty preserves; `remote_pass_clear=1` removes |
| `remote_id` | `[a-z0-9-]{0,24}` | empty = default from MAC |
| `talkback_id` | uint32, default 0 | 0 = polling off |
| `talkback_key` | max 32 printable characters, secret | JSON: `talkback_key_set`; empty preserves; `talkback_key_clear=1` removes |
| `remote_budget_min` | 0..1440, default 60 | UTC daily session budget; 0 = no sessions; unknown clock: 24 h from first usage |
| `remote_idle_min` | 1..60, default 10 | session idle timeout in minutes since the last authenticated valid request |
| `diag_every_h` | 0..168 (default 6) | diagnostic channel every N hours, 0 = every upload |
| `batt_low_mv` | 0 or 2500..4500 (3650) | below: economy mode |
| `batt_crit_mv` | 0 or 2500..4500 (3500), <= low | below: critical mode |
| `batt_capacity_mah` | 100..20000 (2500), integer | cell capacity for consumption-based runtime; stored as `batt_capacity` (NVS key limit), same on PC |
| `update_url` | "" or https:// (PC also http://, mock://) | remote update manifest |
| `update_check_h` | 0..720 (24) | manifest check period, 0 = off |
| `static_ip`, `gateway`, `netmask`, `dns` | "" or a.b.c.d | static IPv4 (empty = DHCP); dns defaults to gateway |
| `log_udp` | 0/1, default 0; JSON boolean | 0 = logs only in the portal view (`/api/logs`), 1 = also send them to `log_dest`; the portal shows one choice: portal only / UDP / MQTT |
| `log_dest` | `udp` or `mqtt`, default `udp` | form and JSON use strings; persistent store uses numeric 0/1 |
| `log_level` | 1..4, default 3 | error / warning / info / debug, for either destination |
| `log_host` | empty or IPv4 | UDP only; empty = LAN broadcast |
| `log_port` | 1..65535, default 5514 | UDP only |

MQTT logging reuses the active remote-session client, never wakes the device
or extends its session/idle limit. `gm/<id>/log` carries unsigned plain text,
QoS 0, without retain; see [remote-protocol.md](remote-protocol.md) for ACLs.
The sender buffers the newest 16 lines (256 bytes each, about 4 KiB) while
offline, then sends batches up to 1024 bytes and a single
`... N lines dropped` notice for accumulated losses. Long lines are truncated.
Transport MQTT/TLS tags are capped at warning (error for `log_level=1`) to
avoid feedback; HTTP debug URLs remain suppressed to protect credentials.
USB and LAN-console tap remain available independently of `log_dest`.

### ThingSpeak diagnostic status

The eight numeric diagnostic fields are unchanged. The complete diagnostic
row's text `status` preserves `fw`, `wake`, `power` and adds whitespace-separated
`capacity=2500 mah=3.50 rt=357.14`. `capacity` is configured mAh; `mah` is
mAh/day and `rt` is remaining days, both with two decimals and omitted when
unknown. The main channel and battery-only historical rows are unchanged.
The status stays within 255 bytes; overflow fails explicitly rather than
truncating keys. `parseStatus` exposes these pairs in `kv`. The Pages dashboard
prefers `rt`; without it, it calculates from `mah`, the row's voltage and
`capacity` (2500 when absent). No usable consumption means collecting data.
Battery-only rows between full diagnostics do not clear the latest full
row's runtime; a newer full row without consumption does clear it.

## Remote update manifest

```json
{"version": "0.2.0",
 "url": "https://example.com/grainmeter-0.2.0-esp32c6-ota.bin",
 "sha256": "<sha256sum of the .bin>",
 "service_id": 7, "service_min": 15}
```

Checked during an upload window every `update_check_h` hours (normal power
mode only). A newer `version` is streamed into the inactive slot through the
same validation as a portal upload, its SHA-256 must match, the version in
the image must equal the manifest; then the device restarts (rollback
protection as for local OTA). `service_id` (optional): each new id opens the
service portal for `service_min` minutes (1..60) once. Extra keys (e.g.
`size`) are ignored. HTTP redirects are followed (max. 5, https only), so
GitHub release URLs such as `/releases/latest/download/manifest.json` work.
`scripts/make_manifest.py` generates the manifest from an OTA image.
`sensor` is the index from `sim.probe.devices`, not the measuring point.

## Remote tunnel

`POST /api/remote/extend` (service password required), form `minutes=1..60`,
extends the active remote session by that many minutes, capped at 60 minutes
from now and the remaining daily budget. It returns
`{"ok":true,"until":<epoch or 0>,"remaining_s":<seconds>,"budget_left_s":<seconds>}`.
Without a remote session: 409 `error:"no_session"`; exhausted budget: 409
`error:"budget"`; invalid minutes: 400. Extension reserves no budget: only
actual battery-powered session time is billed. The deadline is monotonic,
so clock synchronization/correction does not alter the remaining duration.
The retained MQTT status is refreshed without changing nonce or sequence.

`POST /api/service/exit` also ends a remote session. Firmware publishes the
response with QoS 1 and waits up to 2 seconds for its PUBACK before publishing
retained `offline` (also QoS 1, with a bounded PUBACK wait), disconnecting and
releasing the wake lease. Unused time is not billed. A failed/expired PUBACK
wait is logged; delivery cannot be guaranteed when the network is lost.

`GET /api/status` and `/api/diag` include `remote.until` (epoch or 0),
`remote.remaining_s` and `remote.budget_left_s`; these supplement the existing
whole-minute budget fields. The PC simulator supports
`POST /api/sim` with `action=remote_start&minutes=1..60` for session tests.

The transport-independent `gm::PortalTunnel` accepts v1 JSON requests and
returns a JSON envelope for `Portal::handle()`. The full wire format, MAC
inputs, validation order and error codes are in
[remote-protocol.md](remote-protocol.md).

Create it with a `Portal` and its `GrainMeter`, then call `beginSession()`
with a fresh 32-character lowercase hex nonce and the device topic id.
`handle()` returns false for retained messages; otherwise `TunnelOut.payload`
and `len` describe an internal buffer valid until the next call. Publish
`restart` replies before restarting. The caller serializes access and uses
`setBusy()` to reject additional work while a request is queued/in progress.
No network, clock or random-number source is accessed by the tunnel.

Requests use the current service password; a password-changing response is
signed with the password that authenticated that request. Remote MAC failures
have their own 5-attempt / 60-second limiter and do not affect LAN auth.
The last authenticated sequence/message pair is cached, including signed
errors (`501`, `503`, `413`); stale-sequence replies do not replace it.
The response envelope is limited to 20 KiB after JSON escaping/base64 encoding.
Binary history is supported with `"enc":"base64"` and a MAC over the
transmitted base64 text. Only the streaming `POST /api/ota` is unsupported; the four chunked
`/api/ota/*` paths are allowed. Static paths return signed `404`.
The object holds approximately 41 KiB of fixed buffers and should have a
long-lived allocation outside the firmware task stack. No per-request heap
allocation is performed.
For a legacy portal body containing invalid UTF-8, each invalid byte is
represented as `\u00xx`; the response MAC uses the resulting decoded string
encoded as UTF-8. Valid UTF-8 is preserved.

On ESP32 the MQTT adapter is allocated lazily for an enabled session. It uses
one queued request in addition to the request currently running. The MQTT
callback copies the topic suffix, payload and retained flag immediately.
Fragmented messages, payloads over 4096 bytes and messages arriving when the
queue is full are dropped without a response (and counted in `rejected`).
In the last case the browser times out and may retry; the callback never
waits for `DeviceLock`, including during a firmware download. Responses are
copied under `DeviceLock` and published only after releasing it. Keepalive
runs in the MQTT task independently of portal operations. No payload or
broker credentials are logged.

`diag.esp.remote` (also present in platform status) contains `state`
(`idle`, `connecting`, `online`), `id`, `since`, `until` (seconds since this
boot), `requests`, `rejected`, `last_error`, `budget_min`, `budget_left_s`.
The wire status `until` remains an epoch timestamp, or zero without a clock.
The USB/LAN shell supports `remote`, `remote start <1..60>`, `remote stop`.
The main loop checks expiry and bills battery time at its next poll; a portal
operation already writing an image finishes before teardown/restart. A separate lightweight
timer samples USB power every second, independently of `DeviceLock`, including
during blocking uploads/updates. Accounting is flushed under the device lock
at the next opportunity (normally every 60 battery-powered seconds).

Software restart resumes use a separate magic/CRC-protected RTC record;
`RetainedState` does not change. Known update targets are compared with the
running image (including rollback); otherwise the operation succeeds only
when the firmware version changed. A request to restart waits up to 2 seconds
for its response PUBACK before the existing restart path runs.
