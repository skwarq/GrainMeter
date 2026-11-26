# Remote access protocol (MQTT tunnel), gm2

Shared contract: `core/app/portal_tunnel.*`,
`firmware/main/service/remote_session.*`, the PC simulator and
`frontend/src/transport/mqtt*.js`. **gm2 replaces gm1; no backward
compatibility.** Release the firmware and remote frontend together.
Portal endpoints still run through `Portal::handle()`.

## Threat model and limits

Assume the MQTT broker operator/attacker can read, modify, replay, insert
and drop every MQTT message, but does not know the service password
(`svc_pass`). Request and signed response bodies are end-to-end encrypted
and authenticated. Online status is authenticated, not encrypted.
Passwords, Wi-Fi credentials, API keys, OTA chunks and binary history
inside bodies are not exposed to the broker.

This does **not** hide topics/device IDs, methods, paths, status codes,
content types, sizes, timing, online status, names, firmware versions or
session deadlines. It does not protect availability: a broker can disconnect,
suppress replies, forge offline/unsigned errors or replay an old correctly
signed status. A status signature proves origin/integrity, not freshness
against suppression of later status. Restart completion additionally requires
a verified new session nonce and matching operation ID.

PBKDF2 raises the cost of offline guessing; it does not make a weak password
strong. Use a long, random, unique service password. TLS is still mandatory
and certificate verification must remain enabled. This is not protection
against compromised firmware, browser code/XSS, a stolen password or local
storage, or another authenticated service client. `#cfg` connection links
contain secrets: do not share them in logs, chats or tickets.

**Deliberate exception: `gm/<id>/log` stays unsigned and unencrypted.**
Never log secrets. A broker can read, forge and drop log lines.
`GET /api/logs/<since>` through gm2 is encrypted/authenticated instead.

## Broker, topics and ACL

EMQX Cloud Serverless, MQTT 3.1.1, clean session, keep-alive 30 s.
Device: `mqtts://<host>:8883`, CA bundle, client ID `gm-<id>-dev`.
Browser: `wss://<host>:8084/mqtt`, WebSocket subprotocol `mqtt`,
independent random MQTT client ID and application `cid`.
Each client has its own broker login.

`id` is config `remote_id`, `[a-z0-9-]{1,24}` (default `gm-` plus the
last three MAC bytes in hex). `mid` is `[A-Za-z0-9_-]{1,16}`.

| Topic | Direction | QoS | Retained | Payload |
| --- | --- | --- | --- | --- |
| `gm/<id>/status` | device -> clients | 1 | yes | signed online status; unsigned LWT `{"v":2,"state":"offline"}` |
| `gm/<id>/req/<mid>` | client -> device | 1 | **no** | gm2 request |
| `gm/<id>/res/<mid>` | device -> client | 1 | no | gm2 response |
| `gm/<id>/log` | device -> clients/tools | 0 | **no** | unsigned plain UTF-8, <= 1024 B |

| Login | Allow publish | Allow subscribe |
| --- | --- | --- |
| `<id>-dev` | `gm/<id>/res/#`, `gm/<id>/status`, `gm/<id>/log` | `gm/<id>/req/#` |
| `<id>-web` | `gm/<id>/req/#` | `gm/<id>/res/#`, `gm/<id>/status`, `gm/<id>/log` |

Default deny; on EMQX put the deny at the end of each login's rules
(a global All Users deny can override per-login grants).

## Keys and session

A fresh session/reconnect draws a random 128-bit `nonce`, encoded as
32 lowercase hex characters. It resets replay state, lockout and the
response IV counter. A refreshed status within that session preserves nonce
and replay state.

```
N     = 10000
K     = PBKDF2-HMAC-SHA256(UTF8(svc_pass), UTF8("gm2|" + id), N, dkLen=32)
K_mac = HMAC-SHA256(K, UTF8("mac"))       # 32 raw bytes
K_enc = HMAC-SHA256(K, UTF8("enc"))       # 32 raw bytes
```

Keys are cached, calculated at session start or password change, not on
each request. Browser PBKDF2 uses Web Crypto `deriveBits`; deriving is shown
as "Wyliczanie klucza..." / "Deriving key...". Online status advertises
`kdf_iter`. The browser bounds unverified iteration counts to integer
1000..100000 before deriving
and trusts the advertised count only after verifying the resulting signature.

**Hardware acceptance:** 10000 is a conservative initial setting, not a
claimed ESP32-C6 measurement. Firmware measures/logs PBKDF2 milliseconds
once at session start and whenever the password changes, warning above
1000 ms. Measure on the release build/board; adjust N consistently if it
exceeds approximately one second. Check free/minimum heap during TLS,
history and OTA too. No new firmware build is performed in this worktree.

A password-changing request is authenticated and its reply encrypted/signed
with the old keys. Subsequent requests/status use the new password. Refreshing
keys invalidates old duplicate caches but does not reset replay protection
or reuse response IVs. Empty service password disables authenticated access
(`403 no_password`); it must not produce a public empty-password signature.

## Encryption and canonical MAC text

All crypto input strings are UTF-8. `join` below joins fields with exactly
one LF, **no trailing LF**. Numbers use decimal without leading zeros;
`seq` is 1..9007199254740991. `cid` is 16 lowercase hex characters, drawn
randomly per client/tab. `iv` is 32 lowercase hex characters.

The cipher is a domain-separated **HMAC-SHA256 counter stream**:

```
block[i] = HMAC-SHA256(K_enc,
  join(["gm2stream", id, nonce, cid, seq, mid, direction, iv, i]))
ciphertext[j] = plaintext[j] XOR block[floor(j/32)][j % 32]
```

`i` starts at zero, decimal; `direction` is `"req"` or `"res"`.
Browser requests use a fresh random 128-bit IV. Device responses use a
monotonically increasing session counter encoded as 32 hex characters,
including signed errors. The complete context separates clients, sessions,
messages and directions. Never reuse this context for different plaintext;
a duplicate may reuse the exact cached envelope only.

`body` is standard RFC 4648 padded base64 of ciphertext. Authenticate before
decrypting (**Encrypt-then-MAC**), using constant-time MAC comparison.

```
req MAC = hex(HMAC-SHA256(K_mac,
  join(["gm2req", id, nonce, cid, seq, mid, method, path, iv, body])))

res MAC = hex(HMAC-SHA256(K_mac,
  join(["gm2res", id, nonce, cid, seq, mid, status, ctype,
        enc-or-empty, restart ? "1" : "0", iv, body])))
```

MACs are 64 lowercase hex characters. `ctype`, `enc`, `restart` and
ciphertext are all bound to a response. Prefixes bind protocol version.

## Request, response and replay

```json
{"v":2,"cid":"0123456789abcdef","seq":7,"method":"POST","path":"/api/config","iv":"<32 hex>","body":"<ciphertext base64>","mac":"<64 hex>"}
```

Plaintext is the original form-urlencoded body (empty for GET), <= 2048 B.
No query strings. The whole request must fit **4096 B**.

```json
{"v":2,"cid":"0123456789abcdef","seq":7,"status":200,"ctype":"application/json","iv":"<32 hex>","body":"<ciphertext base64>","mac":"<64 hex>","restart":false}
```

For binary history, add `"enc":"base64"`: the **plaintext is raw binary
bytes**, not an additional inner base64 string. The browser verifies and
decrypts to `ArrayBuffer`. Text is decrypted UTF-8 and parsed as portal JSON.
Unknown encodings are rejected. Responses fit **20 KiB**; overflow becomes
a signed, encrypted `413 too_large`.

Device checks: bounded valid JSON/version/fields/topic, retained flag
(drop without reply), password set, global lock, MAC, duplicate/replay,
allowed path and busy state, then portal handler. It remembers independent
`last_seq` and duplicate identity for four LRU `cid`s. A duplicate must
match the authenticated request, not merely seq/mid. Response cache storage
has a shared **14 KiB** budget rather than four full 20 KiB buffers.
Several clients' envelopes coexist while they fit; pressure evicts envelopes
without forgetting request identities. A response larger than this budget
is delivered but not cached.
A valid higher sequence
advances only its client's counter; two tabs start independently at 1.
`409 stale_seq` carries that client's `last_seq`; an unavailable duplicate
cache returns signed `409 duplicate_expired`, without `last_seq`, and must
never re-execute the request or trigger sequence-realignment retries.

Evicted client IDs are retired for the session, preventing recorded
requests from becoming valid again after LRU eviction. A **128-byte Bloom
retirement filter (three hashes)** can conservatively reject a new ID.
Signed `409 client_retired`
lets the browser choose a fresh random `cid` for future requests. It retries
only GET automatically (bounded); POST fails with an ambiguous-result error
rather than risking a second execution after a lost reply. A new session
clears retirement state.

Five bad MACs globally lock all clients for 60 seconds; success/session
start resets failures. This lock is separate from LAN authentication.
Only authenticated requests refresh remote activity.

Unsigned errors keep plaintext JSON-string `body`, `mac:""` and gm2 `v:2`.
They may omit crypto context; trust them only as an error or bounded retry:
`400 bad_request`, `401 bad_mac`, `403 no_password`, `429 locked` (retry_s),
`503 busy`. Oversized/fragmented messages and full one-slot queues get
unsigned errors from the MQTT task without waiting for DeviceLock.
Busy retries back off 0.4/0.8/1.2 seconds with fresh requests. Signed tunnel
errors are encrypted like successful responses.

## Signed online status and restart

```json
{"v":2,"state":"online","nonce":"<32 hex>","fw":"0.4.0","name":"Silos 1","until":1795700000,"remaining_s":600,"budget_left_s":3600,"kdf_iter":10000,"op":null,"mac":"<64 hex>"}
```

`op` is null or `{"mid":"<mid>","fw":"0.4.1","result":"ok"}` (`failed`
on rollback). The canonical signature is:

```
hex(HMAC-SHA256(K_mac, "gm2status\n" + compactJSON(
  [2, id, "online", nonce, fw, name, until, remaining_s, budget_left_s,
   kdf_iter, op ? [op.mid, op.fw, op.result] : null]
)))
```

Use normal JSON string escaping (C++ `JsonWriter`, JS `JSON.stringify`),
no spaces. All important status fields, including the iteration count,
are covered. Unknown extensions must not become trusted data without a
new canonical contract. Unsigned/invalid online status signals availability
only: never use its nonce, deadline, budget or operation outcome.
Offline/LWT remains unsigned availability information.

On `restartAfter`, publish the signed `restart:true` reply QoS 1 and wait
up to two seconds for PUBACK; save the remaining session/operation and
selected boot partition in RTC memory, then restart. Resume with a fresh
nonce; report success according to the booted partition/target, including
rollback and ordinary restart. The browser finishes its operation only
after a verified new-nonce online status with matching `op.mid`. Changing
`restart` or injecting unsigned `op.result` cannot complete it.

## Portal paths, history and OTA

All handled `/api/*` paths are allowed, including `/api/logs/<since>`,
binary `/api/history/<from>/<to>` pages, Wi-Fi and PC `/api/sim`.
Only streaming `POST /api/ota` is excluded (`501 not_supported`).
Static assets are local to the frontend. Wrong Wi-Fi settings can cut the link.

| POST path | Plaintext form | Successful reply |
| --- | --- | --- |
| `/api/ota/begin` | `size=<bytes>&sha256=<64 hex>&name=<filename>` | `{ok:true,chunk:1440,next:0}` |
| `/api/ota/chunk` | `off=<decimal>&d=<base64url without padding>` | `{ok:true,next:<received>}` |
| `/api/ota/finish` | `sha256=<64 hex>` | `{ok:true,version,bytes,restarting:true}`, signed restart |
| `/api/ota/abort` | empty | `{ok:true}` |

**1440 B chunks are unchanged:** 1920 base64url characters plus form fields,
then encrypted/base64, still fit 2048 B plaintext and 4096 B request envelope.
`GET /api/ota` advertises the same chunk size. Duplicates append/hash once;
gaps/partial overlap return signed `409 offset` and `next`. Accepted chunks
or duplicates refresh the 120-second upload idle timer; polls do not.
Finish hashes must match begin and file. Begin shares OTA exclusivity,
rejects Critical power and validates headers before erase.
Firmware extends to 15 minutes within budget, rejects `session_short`
below eight minutes, aborts when the session ends and ticks upload expiry
even while scheduling is paused. Measure encrypted OTA/chunk latency and
heap on hardware; PC tests compare the complete image and history byte-for-byte.

## Session policy and clients

TalkBack wake is unchanged: browser posts `REMOTE <minutes> <valid_until>`
to ThingSpeak (`minutes` 1..60); the device consumes at most one command
per upload/heartbeat window, timeout three seconds, ignoring expired commands
when its wall clock is valid. Device sleeps between windows.

Session daily budget `remote_budget_min` (default 60, 0 disables) is kept
in RTC; USB sessions are not charged. End on deadline, budget, Critical
power, `remote_idle_min` (default 10, 1..60) without authenticated activity,
or `/api/service/exit`. Extend via `POST /api/remote/extend`, `minutes=1..60`,
capped at 60 minutes from now and remaining budget; monotonic deadline,
`until=0` if wall clock unknown. Refreshed signed status keeps the nonce.
Exit sends signed reply -> PUBACK (or logged timeout) -> retained offline
-> PUBACK -> disconnect/release wake lease. PUBACK is broker receipt,
not guaranteed browser delivery.

Remote UI stores credentials in memory unless remembering is explicitly
enabled. `scripts/remote_link.py` is unchanged (only fills the connection
form, no MQTT crypto). Request timeout is 15 seconds, 60 seconds for
update/upload/begin/finish; requests are serialized per tab.
MQTT crypto is imported only by `remote.html`, not the embedded portal;
`portal/index.html.gz` budget remains **72 KiB**.

`tools/mqtt_log.py --config config/device.conf [--status]` uses the web
broker credentials and verified WSS TLS; requires `paho-mqtt>=2,<3`.
It never uses `svc_pass`, publishes requests, wakes or extends sessions.
`--status` prints **UNVERIFIED** raw status, not trusted operation results.
The frontend's optional log subscription denial does not disable gm2.
Logs are plain text (never HTML), stripped of ANSI/control codes; the UI
keeps 500 lines, supports severity filters/pause/clear/text download.
Firmware's 16 x 256 B log queue drops oldest lines and reports losses;
QoS 0 is best effort, without archives or replay.

All portal errors other than the explicitly unsigned transport errors
above remain signed/encrypted with their existing HTTP status and JSON
semantics; see [API.md](API.md). Implementation plan:
[plans/remote-access.md](plans/remote-access.md).
