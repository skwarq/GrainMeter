import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHmac, pbkdf2Sync } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { encodeLength, PacketDecoder, publishPacket, packet, MqttClient } from '../src/transport/mqtt-client.js';
import { MqttTransport, hmac, macText, deriveKeys, statusMacText, cryptBody } from '../src/transport/mqtt.js';
import { request as httpRequest } from '../src/transport/http.js';
import { getHistoryPage, setRemoteTransport } from '../src/api.js';
import { parseHistoryBinary } from '../src/lib/feed.js';
import { sessionSnapshot, remainingSeconds, sessionTime } from '../src/remote/session.js';

const nonce = '0123456789abcdef0123456789abcdef';
const options = { id: 'silo', servicePassword: 'sekret-ą' };
const rawKey = pbkdf2Sync(options.servicePassword, 'gm2|' + options.id, 10000, 32, 'sha256');
const macKey = createHmac('sha256', rawKey).update('mac').digest();
const encKey = createHmac('sha256', rawKey).update('enc').digest();
const sign = (text) => createHmac('sha256', macKey).update(text).digest('hex');
const keys = await deriveKeys(options.servicePassword, options.id, 10000);
function stream(fields, direction, bytes) {
  const output = Buffer.alloc(bytes.length);
  for (let offset = 0; offset < bytes.length; offset += 32) {
    const block = createHmac('sha256', encKey)
      .update(
        [
          'gm2stream',
          options.id,
          fields.nonce ?? nonce,
          fields.cid,
          fields.seq,
          fields.mid ?? fields.topic.split('/').at(-1),
          direction,
          fields.iv,
          offset / 32,
        ].join('\n'),
      )
      .digest();
    for (let j = 0; j < 32 && offset + j < bytes.length; j++) output[offset + j] = bytes[offset + j] ^ block[j];
  }
  return output;
}
async function online(transport, fields = {}) {
  const status = {
    v: 2,
    state: 'online',
    nonce,
    fw: 'test',
    name: 'Silo',
    until: 0,
    remaining_s: 600,
    budget_left_s: 600,
    kdf_iter: 10000,
    ...fields,
  };
  status.mac = sign(statusMacText(options.id, status));
  await transport.receive('gm/silo/status', JSON.stringify(status), true);
}
const tick = () => new Promise((resolve) => setTimeout(resolve, 10));
async function waitSent(client, count) {
  for (let i = 0; i < 200 && client.sent.length < count; i++) await tick();
  assert.equal(client.sent.length, count);
}

class FakeClient {
  sent = [];
  connect() {
    return Promise.resolve();
  }
  subscribe() {
    return Promise.resolve();
  }
  publish(topic, payload) {
    this.sent.push({ topic, ...JSON.parse(payload) });
    return Promise.resolve();
  }
  close() {}
}
function setup() {
  const client = new FakeClient();
  const transport = new MqttTransport(options, client);
  transport.status = { v: 2, state: 'online', nonce, kdf_iter: 10000, verified: true };
  transport.sessionNonce = nonce;
  transport.keyCache = { password: options.servicePassword, iterations: 10000, promise: Promise.resolve(keys) };
  return { client, transport };
}
async function reply(
  transport,
  req,
  { status = 200, data = { ok: true }, body = JSON.stringify(data), enc, mac, restart = false, wire = {} } = {},
) {
  const mid = req.topic.split('/').at(-1);
  const iv = 'ab'.repeat(16);
  const ciphertext =
    mac === ''
      ? body
      : stream({ ...req, iv }, 'res', enc === 'base64' ? Buffer.from(body, 'base64') : Buffer.from(body)).toString(
          'base64',
        );
  const response = {
    v: 2,
    cid: req.cid,
    seq: req.seq,
    status,
    ctype: enc === 'base64' ? 'application/octet-stream' : 'application/json',
    iv,
    body: ciphertext,
    enc,
    restart,
  };
  const signature = sign(macText('res', { ...response, id: options.id, nonce, mid }));
  await transport.receive('gm/silo/res/' + mid, JSON.stringify({ ...response, mac: mac ?? signature, ...wire }), false);
}

test('base64 responses decode all byte values and padding variants after MAC verification', async () => {
  for (const size of [0, 1, 2, 3, 256]) {
    const { client, transport } = setup();
    const bytes = Buffer.from(Array.from({ length: size }, (_, i) => i));
    const result = transport.request('GET', '/api/history/0/4294967295');
    await waitSent(client, 1);
    await reply(transport, client.sent[0], { body: bytes.toString('base64'), enc: 'base64' });
    const data = await result;
    assert.ok(data instanceof ArrayBuffer);
    assert.deepEqual(new Uint8Array(data), new Uint8Array(bytes));
  }
});

test('binary responses reject bad MAC, unknown encoding and malformed base64', async () => {
  for (const response of [
    { body: 'AA==', enc: 'base64', mac: 'f'.repeat(64) },
    { body: 'AA==', enc: 'hex' },
    { body: '{"ok":true}', enc: null },
    { wire: { body: 'AA!' }, enc: 'base64' },
    { wire: { body: 'AA' }, enc: 'base64' },
    { wire: { body: '_w==' }, enc: 'base64' },
  ]) {
    const { client, transport } = setup();
    const result = transport.request('GET', '/api/history/0/1');
    const rejected = assert.rejects(result, (e) => e.kind === 'parse');
    await waitSent(client, 1);
    await reply(transport, client.sent[0], response);
    await rejected;
  }
});

test('binary history errors remain signed JSON errors', async () => {
  const { client, transport } = setup();
  const result = transport.request('GET', '/api/history/0/1');
  const rejected = assert.rejects(result, (e) => e.status === 413 && e.serverMessage === 'too_large');
  await waitSent(client, 1);
  await reply(transport, client.sent[0], { status: 413, data: { error: 'too_large' } });
  await rejected;
});

test('remote getHistoryPage uses integer epochs, forwards abort signal and feeds parseHistoryBinary', async () => {
  const { client, transport } = setup();
  setRemoteTransport(transport);
  try {
    const bytes = Buffer.alloc(40);
    bytes.write('GMH3');
    bytes.writeUInt16LE(1, 4);
    bytes.writeUInt16LE(8, 6);
    bytes.writeUInt32LE(1800000060, 8);
    bytes.writeUInt32LE(12000, 12);
    bytes.writeUInt32LE(1800000000, 16);
    for (let i = 0; i < 8; i++) bytes.writeInt16LE(i === 0 ? -32768 : 2150 + i, 20 + i * 2);
    bytes.writeUInt16LE(3700, 36);
    bytes.writeInt8(-67, 38);
    bytes[39] = 0x25;
    const controller = new AbortController();
    const result = getHistoryPage(1800000000.9, 1800000100.5, controller.signal);
    await waitSent(client, 1);
    assert.equal(client.sent[0].method, 'GET');
    assert.equal(client.sent[0].path, '/api/history/1800000000/1800000100');
    assert.equal(client.sent[0].body, '');
    await reply(transport, client.sent[0], { body: bytes.toString('base64'), enc: 'base64' });
    const page = parseHistoryBinary(await result);
    assert.equal(page.next, 1800000060);
    assert.equal(page.maxGapMs, 12000000);
    assert.equal(page.entries.length, 1);
    assert.equal(page.entries[0].t, 1800000000000);
    assert.deepEqual(page.entries[0].values, [null, 21.51, 21.52, 21.53, 21.54, 21.55, 21.56, 21.57]);
    assert.equal(page.entries[0].diag.batteryMv, 3700);
    assert.equal(page.entries[0].diag.rssi, -67);
    assert.equal(page.entries[0].restart, true);
    const aborted = getHistoryPage(0, 1, controller.signal);
    const rejected = assert.rejects(aborted, (e) => e.kind === 'aborted');
    await waitSent(client, 2);
    controller.abort();
    await rejected;
  } finally {
    setRemoteTransport(null);
    transport.close();
  }
});

test('session bar counts down epoch and unknown-clock response without going negative', () => {
  const epoch = sessionSnapshot({ until: 1800000060, remaining_s: 999, budget_left_s: 120 }, 1800000000000);
  assert.equal(remainingSeconds(epoch, 1800000000000), 60);
  assert.equal(sessionTime(remainingSeconds(epoch, 1800000011000)), '0:49');
  assert.equal(remainingSeconds(epoch, 1800000070000), 0);
  const unknown = sessionSnapshot({ until: 0, remaining_s: 61, budget_left_s: 125 }, 1000);
  assert.equal(sessionTime(remainingSeconds(unknown, 2000)), '1:00');
  assert.equal(sessionTime(unknown.budgetLeft), '2:05');
  assert.equal(remainingSeconds(sessionSnapshot({ until: 0 }), 2000), null);
});

test('session extension refreshes the bar from a signed response and retained status keeps sequence', async () => {
  const { client, transport } = setup();
  let session = sessionSnapshot({ until: 0, remaining_s: 60, budget_left_s: 600 }, 1000);
  transport.onresponse = (data) => {
    session = sessionSnapshot(data, 2000);
  };
  transport.onstatus = (data) => {
    session = sessionSnapshot(data, 2000);
  };
  const pending = transport.request('POST', '/api/remote/extend', { form: { minutes: 10 } });
  await waitSent(client, 1);
  assert.equal(stream(client.sent[0], 'req', Buffer.from(client.sent[0].body, 'base64')).toString(), 'minutes=10');
  await reply(transport, client.sent[0], { data: { ok: true, until: 0, remaining_s: 600, budget_left_s: 600 } });
  await pending;
  assert.equal(remainingSeconds(session, 7000), 595);
  assert.equal(session.budgetLeft, 600);
  await online(transport, { until: 1800000600, budget_left_s: 590 });
  assert.equal(remainingSeconds(session, 1800000000000), 600);
  assert.equal(transport.seq, 1);
});

test('ending the session goes offline only after a verified successful reply', async () => {
  const { client, transport } = setup();
  const states = [];
  transport.onstatus = (s) => states.push(s.state);
  const failed = transport.request('POST', '/api/service/exit');
  const rejection = assert.rejects(failed, (e) => e.status === 409);
  await waitSent(client, 1);
  await reply(transport, client.sent[0], { status: 409, data: { error: 'update in progress' } });
  await rejection;
  assert.equal(transport.status.state, 'online');
  const pending = transport.request('POST', '/api/service/exit');
  await waitSent(client, 2);
  assert.deepEqual(states, []);
  await reply(transport, client.sent[1]);
  assert.deepEqual(await pending, { ok: true });
  assert.deepEqual(states, ['offline']);
  await assert.rejects(transport.request('GET', '/api/status'), (e) => e.kind === 'network');
});

test('MQTT variable lengths and fragmented/coalesced QoS1 PUBLISH', () => {
  for (const [n, expected] of [
    [0, [0]],
    [127, [127]],
    [128, [128, 1]],
    [16384, [128, 128, 1]],
    [268435455, [255, 255, 255, 127]],
  ])
    assert.deepEqual(encodeLength(n), expected);
  assert.throws(() => encodeLength(268435456));
  const p = publishPacket('gm/silo/req/a', 'ą'.repeat(100), 0x1234);
  const d = new PacketDecoder();
  assert.deepEqual(d.push(p.slice(0, 2)), []);
  assert.deepEqual(d.push(p.slice(2, 7)), []);
  const packets = d.push(new Uint8Array([...p.slice(7), ...packet(0xd0)]));
  assert.equal(packets[0].header, 0x32);
  const b = packets[0].body,
    offset = 2 + b[0] * 256 + b[1];
  assert.deepEqual([...b.slice(offset, offset + 2)], [0x12, 0x34]);
  assert.equal(packets[1].header, 0xd0);
  assert.throws(() => new PacketDecoder().push(Uint8Array.from([0x30, 128, 128, 128, 128])));
});

test('WebSocket handshake, subscriptions, PUBACK in both directions and disconnect', async () => {
  class WS {
    readyState = 1;
    sent = [];
    constructor(url, protocol) {
      assert.equal(url, 'wss://broker/mqtt');
      assert.equal(protocol, 'mqtt');
      WS.instance = this;
    }
    send(p) {
      this.sent.push(p);
    }
    close() {
      this.readyState = 3;
      this.onclose?.();
    }
    receive(p) {
      this.onmessage({ data: p.buffer });
    }
  }
  const client = new MqttClient({
    url: 'wss://broker/mqtt',
    username: 'user',
    password: 'pass',
    id: 'silo',
    WebSocketImpl: WS,
  });
  const connected = client.connect(),
    ws = WS.instance;
  ws.onopen();
  const connect = new PacketDecoder().push(ws.sent[0])[0];
  assert.equal(connect.header, 0x10);
  assert.equal(connect.body[7], 0xc2);
  assert.equal(connect.body[9], 30);
  assert.match(new TextDecoder().decode(connect.body), /gm-silo-web-[a-f0-9]{6}/);
  ws.receive(packet(0x20, [0, 0]));
  await connected;
  const subscribed = client.subscribe('gm/silo/status');
  ws.receive(packet(0x90, [0, 1, 1]));
  await subscribed;
  const published = client.publish('gm/silo/req/a', '{}');
  ws.receive(packet(0x40, [0, 2]));
  await published;
  let incoming;
  client.onmessage = (...args) => {
    incoming = args;
  };
  ws.receive(publishPacket('gm/silo/status', '{}', 99));
  assert.deepEqual(incoming, ['gm/silo/status', '{}', false]);
  assert.deepEqual([...ws.sent.at(-1)], [0x40, 2, 0, 99]);
  client.close();
  assert.deepEqual([...ws.sent.at(-1)], [0xe0, 0]);
});

test('request and response MAC agree with independent node:crypto vectors', async () => {
  const req = {
    id: 'silo',
    nonce,
    cid: '12'.repeat(8),
    seq: 7,
    mid: 'a_b',
    method: 'POST',
    path: '/api/config',
    iv: 'ab'.repeat(16),
    body: 'AA==',
  };
  const requestVector = ['gm2req', 'silo', nonce, req.cid, '7', 'a_b', 'POST', '/api/config', req.iv, 'AA=='].join(
    '\n',
  );
  assert.equal(await hmac(keys.mac, macText('req', req)), sign(requestVector));
  const res = { ...req, status: 200, ctype: 'application/json', restart: true };
  const responseVector = [
    'gm2res',
    'silo',
    nonce,
    req.cid,
    '7',
    'a_b',
    '200',
    'application/json',
    '',
    '1',
    req.iv,
    'AA==',
  ].join('\n');
  assert.equal(await hmac(keys.mac, macText('res', res)), sign(responseVector));
});

test('gm2 PBKDF2, subkeys, stream and status MAC match shared C++ known vectors', async () => {
  const vector = JSON.parse(await readFile(new URL('../../tests/data/gm2-vectors.json', import.meta.url), 'utf8'));
  const derived = await deriveKeys(vector.password, vector.id, vector.kdf_iter);
  const raw = pbkdf2Sync(vector.password, 'gm2|' + vector.id, vector.kdf_iter, 32, 'sha256');
  assert.equal(raw.toString('hex'), vector.K);
  assert.equal(createHmac('sha256', raw).update('mac').digest('hex'), vector.Kmac);
  assert.equal(createHmac('sha256', raw).update('enc').digest('hex'), vector.Kenc);
  for (const kind of ['req', 'res']) {
    const fields = { ...vector[kind], id: vector.id, nonce: vector.nonce };
    assert.equal(macText(kind, fields), fields.text);
    assert.equal(await hmac(derived.mac, fields.text), fields.mac);
    const plain = Buffer.from(fields.plaintext_hex, 'hex');
    assert.equal(Buffer.from(await cryptBody(derived.enc, fields, kind, plain)).toString('base64'), fields.body);
    assert.deepEqual(
      Buffer.from(await cryptBody(derived.enc, fields, kind, Buffer.from(fields.body, 'base64'))),
      plain,
    );
  }
  assert.equal(statusMacText(vector.id, vector.status.fields), vector.status.text);
  assert.equal(await hmac(derived.mac, vector.status.text), vector.status.mac);
});

test('key change response verifies with old request key while pending status uses new password', async () => {
  const { client, transport } = setup();
  const pending = transport.request('POST', '/api/config', { form: { password: 'updated' } });
  await waitSent(client, 1);
  transport.setPassword('updated');
  await reply(transport, client.sent[0]);
  assert.deepEqual(await pending, { ok: true });
});

test('one in flight, increasing seq, verified responses', async () => {
  const { client, transport } = setup();
  const one = transport.request('GET', '/api/status');
  const two = transport.request('GET', '/api/diag');
  await waitSent(client, 1);
  assert.equal(client.sent.length, 1);
  const req = client.sent[0],
    mid = req.topic.split('/').at(-1);
  assert.equal(req.mac, sign(['gm2req', 'silo', nonce, req.cid, 1, mid, 'GET', '/api/status', req.iv, ''].join('\n')));
  await reply(transport, req);
  assert.deepEqual(await one, { ok: true });
  await waitSent(client, 2);
  assert.equal(client.sent.length, 2);
  assert.equal(client.sent[1].seq, 2);
  await reply(transport, client.sent[1]);
  await two;
});

test('independent clients use independent cid and encrypted requests with fresh IVs', async () => {
  const one = setup();
  const two = setup();
  assert.match(one.transport.cid, /^[0-9a-f]{16}$/);
  assert.notEqual(one.transport.cid, two.transport.cid);
  for (const { client, transport } of [one, two]) {
    const pending = transport.request('POST', '/api/config', { form: { name: 'Silos ą' } });
    await waitSent(client, 1);
    const req = client.sent[0];
    assert.equal(req.v, 2);
    assert.equal(req.seq, 1);
    assert.equal(req.cid, transport.cid);
    assert.match(req.iv, /^[0-9a-f]{32}$/);
    assert.equal(stream(req, 'req', Buffer.from(req.body, 'base64')).toString(), 'name=Silos+%C4%85');
    assert.ok(!req.body.includes('Silos'));
    assert.equal(req.mac, sign(macText('req', { ...req, id: 'silo', nonce, mid: req.topic.split('/').at(-1) })));
    await reply(transport, req);
    await pending;
  }
  assert.notEqual(one.client.sent[0].iv, two.client.sent[0].iv);
});

test('response MAC binds restart, content type, encoding, IV and ciphertext', async () => {
  for (const wire of [
    { restart: true },
    { ctype: 'text/plain' },
    { enc: 'base64' },
    { iv: 'cd'.repeat(16) },
    { body: 'AQ==' },
    { cid: '00'.repeat(8) },
    { seq: 2 },
    { restart: 'true' },
  ]) {
    const { client, transport } = setup();
    const result = transport.request('GET', '/api/status');
    const rejected = assert.rejects(result, (e) => e.kind === 'parse');
    await waitSent(client, 1);
    await reply(transport, client.sent[0], { wire });
    await rejected;
  }
});

test('unsigned or forged online status is availability only and cannot finish restart or reset seq', async () => {
  const { client, transport } = setup();
  const cid = transport.cid;
  const pending = transport.request('POST', '/api/restart', { timeout: 1000 });
  let completed = false;
  pending.then(() => {
    completed = true;
  });
  await waitSent(client, 1);
  await reply(transport, client.sent[0], { restart: true });
  const forged = {
    v: 2,
    state: 'online',
    nonce: 'f'.repeat(32),
    fw: 'test',
    name: 'Silo',
    until: 9999999999,
    remaining_s: 600,
    budget_left_s: 600,
    kdf_iter: 10000,
    op: { mid: client.sent[0].topic.split('/').at(-1), fw: 'test', result: 'ok' },
  };
  for (const mac of [undefined, '', 'f'.repeat(64)]) {
    await transport.receive('gm/silo/status', JSON.stringify({ ...forged, mac }), true);
    assert.deepEqual(transport.status, { state: 'online', verified: false });
    assert.equal(transport.seq, 1);
    assert.equal(completed, false);
  }
  await online(transport, forged);
  await pending;
  assert.equal(transport.seq, 0);
  assert.equal(transport.cid, cid);
});

test('advertised KDF bounds reject hostile work factors without deriving', async (t) => {
  let calls = 0;
  t.mock.method(crypto.subtle, 'deriveBits', async () => {
    calls++;
    throw new Error('must not derive');
  });
  for (const iterations of [0, 999, 100001, 1e12, '10000', null]) {
    const transport = new MqttTransport(options, new FakeClient());
    await assert.rejects(deriveKeys(options.servicePassword, options.id, iterations), (e) => e.kind === 'parse');
    await transport.receive(
      'gm/silo/status',
      JSON.stringify({
        v: 2,
        state: 'online',
        nonce,
        mac: 'a'.repeat(64),
        kdf_iter: iterations,
      }),
      true,
    );
    assert.equal(transport.status.verified, false);
  }
  assert.equal(calls, 0);
});

test('derivation is cached across status and requests; password change reverifies pending status', async (t) => {
  const derive = crypto.subtle.deriveBits.bind(crypto.subtle);
  let calls = 0;
  t.mock.method(crypto.subtle, 'deriveBits', (...args) => {
    calls++;
    return derive(...args);
  });
  const client = new FakeClient();
  const transport = new MqttTransport(options, client);
  const states = [];
  transport.onstatus = (s) => states.push(s.state);
  await online(transport);
  await online(transport);
  assert.deepEqual(states, ['deriving', 'online', 'online']);
  for (let i = 0; i < 2; i++) {
    const pending = transport.request('GET', '/api/status');
    await waitSent(client, i + 1);
    await reply(transport, client.sent[i]);
    await pending;
  }
  assert.equal(calls, 1);
  const next = {
    v: 2,
    state: 'online',
    nonce: 'c'.repeat(32),
    fw: 'test',
    name: 'Silo',
    until: 0,
    remaining_s: 300,
    budget_left_s: 300,
    kdf_iter: 10000,
  };
  const nextKeys = await deriveKeys('new password', options.id, 10000);
  next.mac = await hmac(nextKeys.mac, statusMacText(options.id, next));
  await transport.receive('gm/silo/status', JSON.stringify(next), true);
  assert.equal(transport.status.verified, false);
  transport.setPassword('new password');
  for (let i = 0; i < 200 && !transport.status.verified; i++) await tick();
  assert.equal(transport.status.nonce, next.nonce);
  assert.equal(transport.seq, 0);
});

test('late status verification cannot override a newer status, password change or disconnect', async () => {
  for (const action of ['status', 'password', 'disconnect']) {
    const { transport } = setup();
    let release;
    transport.keys = () =>
      new Promise((resolve) => {
        release = resolve;
      });
    const s = {
      v: 2,
      state: 'online',
      nonce: 'b'.repeat(32),
      fw: 'test',
      name: 'Silo',
      until: 0,
      remaining_s: 600,
      budget_left_s: 600,
      kdf_iter: 10000,
    };
    s.mac = sign(statusMacText(options.id, s));
    const pending = transport.receive('gm/silo/status', JSON.stringify(s), true);
    const oldRelease = release;
    if (action === 'status') await transport.receive('gm/silo/status', '{"v":2,"state":"offline"}', true);
    else if (action === 'password') transport.setPassword('changed');
    else transport.client.onclose();
    oldRelease(keys);
    await pending;
    assert.notEqual(transport.status.nonce, s.nonce);
    assert.equal(transport.seq, 0);
    if (action === 'password') {
      release(keys);
      await tick();
    }
  }
});

test('signed client_retired rotates cid and retries GET bounded; unsigned errors never rotate cid', async () => {
  const { client, transport } = setup();
  const pending = transport.request('GET', '/api/status');
  const rejected = assert.rejects(pending, (e) => e.serverMessage === 'client_retired');
  const cids = new Set();
  for (let i = 0; i < 4; i++) {
    await waitSent(client, i + 1);
    assert.equal(client.sent[i].seq, 1);
    cids.add(client.sent[i].cid);
    await reply(transport, client.sent[i], { status: 409, data: { error: 'client_retired' } });
  }
  await rejected;
  assert.equal(cids.size, 4);
  const unsigned = transport.request('GET', '/api/status');
  const untrusted = assert.rejects(unsigned, (e) => e.kind === 'untrusted' && e.serverMessage === 'untrusted');
  await waitSent(client, 5);
  const cid = transport.cid;
  await reply(transport, client.sent[4], {
    status: 409,
    data: { error: 'client_retired' },
    mac: '',
    wire: { cid: undefined, iv: undefined },
  });
  await untrusted;
  assert.equal(transport.cid, cid);
});

test('retired POST rejects its ambiguous result without replay; future requests use a fresh cid', async () => {
  const { client, transport } = setup();
  const cid = transport.cid;
  const pending = transport.request('POST', '/api/measure');
  const rejected = assert.rejects(pending, (e) => e.status === 409 && e.serverMessage === 'client_retired');
  await waitSent(client, 1);
  await reply(transport, client.sent[0], { status: 409, data: { error: 'client_retired' } });
  await rejected;
  await tick();
  assert.equal(client.sent.length, 1);
  assert.notEqual(transport.cid, cid);
  assert.equal(transport.seq, 0);
  const next = transport.request('GET', '/api/status');
  await waitSent(client, 2);
  assert.equal(client.sent[1].cid, transport.cid);
  assert.equal(client.sent[1].seq, 1);
  await reply(transport, client.sent[1]);
  assert.deepEqual(await next, { ok: true });
});

test('signed duplicate_expired never automatically replays GET or POST', async () => {
  for (const method of ['GET', 'POST']) {
    const { client, transport } = setup();
    const cid = transport.cid;
    const pending = transport.request(method, '/api/status');
    const rejected = assert.rejects(pending, (e) => e.status === 409 && e.serverMessage === 'duplicate_expired');
    await waitSent(client, 1);
    await reply(transport, client.sent[0], { status: 409, data: { error: 'duplicate_expired' } });
    await rejected;
    await tick();
    assert.equal(client.sent.length, 1);
    assert.equal(transport.cid, cid);
    assert.equal(transport.seq, 1);
  }
});

test('request plaintext and wire limits preserve 1440-byte OTA chunks and reject oversized responses', async () => {
  const { client, transport } = setup();
  await assert.rejects(
    transport.request('POST', '/api/config', { form: { x: 'a'.repeat(2047) } }),
    (e) => e.status === 413,
  );
  await assert.rejects(transport.request('GET', '/api/' + 'a'.repeat(4096)), (e) => e.status === 413);
  assert.equal(client.sent.length, 0);
  const pending = transport.request('POST', '/api/ota/chunk', {
    form: { off: 0, d: Buffer.alloc(1440).toString('base64url') },
  });
  await waitSent(client, 1);
  assert.ok(new TextEncoder().encode(JSON.stringify(client.sent[0])).length < 4096);
  await reply(transport, client.sent[0]);
  await pending;
  const oversized = transport.request('GET', '/api/history/0/1');
  const rejected = assert.rejects(oversized, (e) => e.kind === 'parse');
  await waitSent(client, 2);
  client.onmessage(
    client.sent[1].topic.replace('/req/', '/res/'),
    JSON.stringify({
      v: 2,
      body: 'a'.repeat(20 * 1024),
    }),
    false,
  );
  await rejected;
});

test('signed stale_seq realigns up to three times; the fourth conflict is returned', async () => {
  const { client, transport } = setup();
  const result = transport.request('GET', '/api/status');
  const rejected = assert.rejects(result, (e) => e.status === 409);
  const last = [19, 25, 31, 40];
  for (let i = 0; i < 4; i++) {
    await waitSent(client, i + 1);
    if (i) assert.equal(client.sent[i].seq, last[i - 1] + 1);
    await reply(transport, client.sent[i], { status: 409, data: { error: 'stale_seq', last_seq: last[i] } });
  }
  await rejected;
  assert.equal(client.sent.length, 4);
});

test('unsigned busy backs off and retries with a new seq, then gives up', async () => {
  const { client, transport } = setup();
  const result = transport.request('GET', '/api/status');
  await waitSent(client, 1);
  await reply(transport, client.sent[0], { status: 503, data: { error: 'busy' }, mac: '' });
  await waitSent(client, 2);
  assert.equal(client.sent[1].seq, client.sent[0].seq + 1);
  await reply(transport, client.sent[1], { status: 200, data: { ok: true } });
  assert.deepEqual(await result, { ok: true });

  const failing = transport.request('GET', '/api/status');
  const rejected = assert.rejects(failing, (e) => e.kind === 'untrusted' && e.serverMessage === 'busy');
  for (let i = 2; i < 6; i++) {
    await waitSent(client, i + 1);
    await reply(transport, client.sent[i], { status: 503, data: { error: 'busy' }, mac: '' });
  }
  await rejected;
  assert.equal(client.sent.length, 6);
});

test('bad MAC is rejected and unsigned errors expose only allowlisted code', async () => {
  for (const [mac, expected] of [
    ['f'.repeat(64), 'parse'],
    ['', 'untrusted'],
  ]) {
    const { client, transport } = setup();
    const result = transport.request('GET', '/api/status');
    const rejected = assert.rejects(
      result,
      (e) => e.kind === expected && (expected !== 'untrusted' || (e.serverMessage === 'bad_mac' && e.status === 0)),
    );
    await waitSent(client, 1);
    await reply(transport, client.sent[0], {
      status: 401,
      data: { error: 'bad_mac', message: 'untrusted detail' },
      mac,
    });
    await rejected;
  }
});

test('timeout releases the queue and aborted queued calls are skipped', async () => {
  const { client, transport } = setup();
  const controller = new AbortController();
  const first = transport.request('GET', '/api/status', { timeoutOverride: 10 });
  const second = transport.request('GET', '/api/diag', { signal: controller.signal });
  controller.abort();
  await Promise.all([
    assert.rejects(first, (e) => e.kind === 'timeout'),
    assert.rejects(second, (e) => e.kind === 'aborted'),
  ]);
  assert.equal(client.sent.length, 1);
});

test('restart waits for matching op.mid and a fresh nonce; resets sequence', async () => {
  const { client, transport } = setup();
  const result = transport.request('POST', '/api/restart');
  let done = false;
  result.then(() => {
    done = true;
  });
  await waitSent(client, 1);
  await reply(transport, client.sent[0], { restart: true });
  await tick();
  assert.equal(done, false);
  const status = { nonce: 'a'.repeat(32), op: { mid: 'other', fw: 'test', result: 'ok' } };
  await online(transport, status);
  assert.equal(done, false);
  status.op.mid = client.sent[0].topic.split('/').at(-1);
  await online(transport, status);
  assert.deepEqual(await result, { ok: true });
  assert.equal(transport.seq, 0);
});

test('OTA finish waits for its restart op; rollback result is not reported as success', async () => {
  for (const outcome of ['ok', 'rollback']) {
    const { client, transport } = setup();
    const result = transport.request('POST', '/api/ota/finish', { form: { sha256: 'hash' } });
    const settled =
      outcome === 'ok' ? result : assert.rejects(result, (e) => e.status === 422 && e.serverMessage === 'rollback');
    await waitSent(client, 1);
    await reply(transport, client.sent[0], { restart: true, data: { ok: true, restarting: true } });
    await online(transport, {
      nonce: 'b'.repeat(32),
      op: { mid: client.sent[0].topic.split('/').at(-1), fw: 'test', result: outcome },
    });
    await settled;
  }
});

test('signed OTA offset error preserves next and explicit timeout is honoured', async () => {
  const { client, transport } = setup();
  const chunk = transport.request('POST', '/api/ota/chunk', { form: { off: 0, d: '6Q' } });
  const rejected = assert.rejects(chunk, (e) => e.status === 409 && e.serverMessage === 'offset' && e.next === 1440);
  await waitSent(client, 1);
  await reply(transport, client.sent[0], { status: 409, data: { error: 'offset', next: 1440 } });
  await rejected;
  await assert.rejects(transport.request('POST', '/api/ota/begin', { timeout: 5 }), (e) => e.kind === 'timeout');
});

test('OTA status reconnects the broker and waits for device online before resuming requests', async () => {
  const { client, transport } = setup();
  client.ws = { readyState: 3 };
  transport.status = { state: 'offline' };
  let connects = 0;
  const subscribed = [];
  client.connect = async () => {
    connects++;
    client.ws.readyState = 1;
  };
  client.subscribe = async (topic) => subscribed.push(topic);
  const result = transport.request('GET', '/api/ota', { reconnect: true });
  await tick();
  assert.equal(client.sent.length, 0);
  await online(transport);
  await waitSent(client, 1);
  await reply(transport, client.sent[0], { data: { received: 1440, expected: 3001 } });
  assert.equal((await result).received, 1440);
  assert.equal(connects, 1);
  assert.deepEqual(subscribed, ['gm/silo/res/#', 'gm/silo/status', 'gm/silo/log']);
});

test('cancellation during offline recovery releases the queue for device abort', async () => {
  const { client, transport } = setup();
  transport.status = { state: 'offline' };
  const controller = new AbortController();
  const result = transport.request('GET', '/api/ota', { reconnect: true, signal: controller.signal });
  const rejected = assert.rejects(result, (e) => e.kind === 'aborted');
  await tick();
  controller.abort();
  await rejected;
  const abort = transport.request('POST', '/api/ota/abort', { reconnect: true });
  await online(transport);
  await waitSent(client, 1);
  assert.equal(client.sent[0].path, '/api/ota/abort');
  await reply(transport, client.sent[0]);
  await abort;
});

test('HTTP keeps headers, form encoding, timeout and errors', async (t) => {
  t.mock.method(globalThis, 'fetch', async (path, options) => {
    assert.equal(path, '/api/config');
    assert.equal(options.headers['X-GM-Password'], 'secret');
    assert.equal(options.body.toString(), 'name=Silos+1');
    assert.equal(options.credentials, 'same-origin');
    assert.equal(options.cache, 'no-store');
    return new Response('{"ok":true}');
  });
  assert.deepEqual(await httpRequest('POST', '/api/config', { password: 'secret', form: { name: 'Silos 1' } }), {
    ok: true,
  });
  globalThis.fetch = async () => new Response('{"error":"bad"}', { status: 400 });
  await assert.rejects(httpRequest('GET', '/api/status'), (e) => e.status === 400 && e.serverMessage === 'bad');
  globalThis.fetch = (_path, options) =>
    new Promise((_resolve, reject) => options.signal.addEventListener('abort', () => reject(new Error())));
  await assert.rejects(httpRequest('GET', '/api/status', { timeout: 5 }), (e) => e.kind === 'timeout');
});

test('HTTP OTA offset errors preserve authoritative next for the uploader', async (t) => {
  t.mock.method(globalThis, 'fetch', async () => new Response('{"error":"offset","next":1440}', { status: 409 }));
  await assert.rejects(
    httpRequest('POST', '/api/ota/chunk', { form: { off: 0, d: '6Q' } }),
    (e) => e.status === 409 && e.serverMessage === 'offset' && e.next === 1440,
  );
});
