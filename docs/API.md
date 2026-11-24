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
| GET | `/api/config` | configuration without secrets |
| GET | `/api/ota` | state of the current/last firmware upload |
| GET | `/api/history` | local history summary (JSON, below) |
| GET | `/api/history/<from>/<to>` | local history page (binary, below) |

### `/api/status` fields

`firmware`, `platform` (`"pc-sim"` or `"esp32c6"`), `name`, `mono_s`,
`time_valid`, `epoch` (UTC, 0 if unknown), `wake_reason`, `reset_reason`,
`battery_mv` (number or `null`), `wifi{state,ssid,rssi}`, `config_ok`,
`upload_configured`, `schedule{measure_s,upload_s,samples_per_upload,
next_measure_in_s,upload_due}`, `measurement` (or `null`), `buffer`,
`last_upload` (or `null`), `power`, `remote_update`, `errors[]`, `ota`, and
a platform object: `sim` on PC, `esp` on the ESP32.

`power`: `mode` (`normal` / `economy` / `critical`), `battery_mv` (or
`null`), `low_mv`, `crit_mv` (0 = threshold disabled). Economy: uploads 4x
less often. Critical: no radio at all, measuring interval doubled.

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

`sim` (PC only): `speed`, `network_up`, `lost_response`, `http_posts`,
`mock_entries_main`, `mock_entries_diag`, `config_file`,
`ota_slots{running,pending_new_image,last_boot,rejected,fail_next_boot}`,
`battery_override_mv` (0 = simulated curve), `service_requests`,
`time_syncs`, `http_sessions`,
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
| `/api/config` | any of `name, wifi_ssid, wifi_pass, wifi_pass_clear, report_url, ts_channel, ts_key, diag_channel, diag_key, measure_s, upload_s, svc_pass, diag_every_h, batt_low_mv, batt_crit_mv, update_url, update_check_h, static_ip, gateway, netmask, dns` | validates all, saves (NVS / file); empty secret = keep |
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

### Firmware upload `POST /api/ota`

Either a normal HTML file form (`multipart/form-data`, first file part is
used; any file name) or the raw image (`application/octet-stream`, optional
`X-File-Name`). The content decides: ESP-IDF image magic, chip ESP32-C6,
project `grainmeter`, size within the OTA slot. The image is streamed to the
inactive slot and selected for boot only after full validation; then the
reply `{ok:true, version, bytes}` is sent and the device restarts. Errors:
`401`, `409` (busy), `411`, `413` (too large), `415` (not a GrainMeter
ESP32-C6 image), `422` (image verification failed), `500` (flash error).

### Simulation `POST /api/sim` (PC)

`action=` `crc` (`sensor`, `count` = reads, -1 = permanent), `remove` /
`restore` (`sensor`), `temp` (`sensor`, `value` °C), `unsupported`
(`arg=on|off`, adds a DS18S20 0x10), `short` (`arg=on|off`, DATA stuck low),
`net` (`arg=down|up`), `lost_response` (`arg=on|off`: server stores, reply
lost), `speed` (`value`), `advance` (`seconds`), `powerloss`, `battery`
(`value` mV, 0 = simulated curve), `ota_fail_boot` (`arg=on|off`: the next
new image fails its first start and is rolled back).

## Configuration fields (new in v0.2)

| Field | Range | Meaning |
| --- | --- | --- |
| `diag_every_h` | 0..168 (default 6) | diagnostic channel every N hours, 0 = every upload |
| `batt_low_mv` | 0 or 2500..4500 (3450) | below: economy mode |
| `batt_crit_mv` | 0 or 2500..4500 (3300), <= low | below: critical mode |
| `update_url` | "" or https:// (PC also http://, mock://) | remote update manifest |
| `update_check_h` | 0..720 (24) | manifest check period, 0 = off |
| `static_ip`, `gateway`, `netmask`, `dns` | "" or a.b.c.d | static IPv4 (empty = DHCP); dns defaults to gateway |

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
