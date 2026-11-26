import { MqttClient } from './mqtt-client.js';
import { ApiError } from '../lib/errors.js';
import { RemoteLogs } from '../remote/logs.js';

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });
const hex = (bytes) => Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
const randomHex = (size) => hex(crypto.getRandomValues(new Uint8Array(size)));
const validHex = (value, size) => typeof value === 'string' && new RegExp(`^[0-9a-f]{${size}}$`).test(value);
export function macText(kind, { id, nonce, cid, seq, mid, method, path, status, ctype, enc, restart, iv, body }) {
  return [
    kind === 'req' ? 'gm2req' : 'gm2res',
    id,
    nonce,
    cid,
    seq,
    mid,
    ...(kind === 'req' ? [method, path] : [status, ctype, enc || '', restart ? '1' : '0']),
    iv,
    body,
  ].join('\n');
}
export function statusMacText(id, s) {
  return (
    'gm2status\n' +
    JSON.stringify([
      2,
      id,
      'online',
      s.nonce,
      s.fw,
      s.name,
      s.until,
      s.remaining_s,
      s.budget_left_s,
      s.kdf_iter,
      s.op ? [s.op.mid, s.op.fw, s.op.result] : null,
    ])
  );
}
async function importHmac(bytes) {
  return crypto.subtle.importKey('raw', bytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
}
async function signBytes(key, text) {
  return new Uint8Array(await crypto.subtle.sign('HMAC', key, encoder.encode(text)));
}
export async function hmac(key, text) {
  if (!(key instanceof CryptoKey)) key = await importHmac(typeof key === 'string' ? encoder.encode(key) : key);
  return hex(await signBytes(key, text));
}
export async function deriveKeys(password, id, iterations) {
  if (!Number.isInteger(iterations) || iterations < 1000 || iterations > 100000) throw new ApiError('parse');
  const passwordKey = await crypto.subtle.importKey('raw', encoder.encode(password), 'PBKDF2', false, ['deriveBits']);
  const raw = new Uint8Array(
    await crypto.subtle.deriveBits(
      { name: 'PBKDF2', hash: 'SHA-256', salt: encoder.encode('gm2|' + id), iterations },
      passwordKey,
      256,
    ),
  );
  const key = await importHmac(raw);
  const mac = await importHmac(await signBytes(key, 'mac'));
  const enc = await importHmac(await signBytes(key, 'enc'));
  return { mac, enc };
}
export async function cryptBody(key, { id, nonce, cid, seq, mid, iv }, direction, bytes) {
  const output = new Uint8Array(bytes.length);
  for (let offset = 0; offset < bytes.length; offset += 32) {
    const block = await signBytes(key, ['gm2stream', id, nonce, cid, seq, mid, direction, iv, offset / 32].join('\n'));
    for (let j = 0; j < 32 && offset + j < bytes.length; j++) output[offset + j] = bytes[offset + j] ^ block[j];
  }
  return output;
}
function base64(bytes) {
  return btoa(Array.from(bytes, (b) => String.fromCharCode(b)).join(''));
}
function unbase64(text) {
  if (!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(text)) throw new ApiError('parse');
  const bytes = Uint8Array.from(atob(text), (c) => c.charCodeAt(0));
  if (base64(bytes) !== text) throw new ApiError('parse');
  return bytes;
}
export function equalMac(a, b) {
  if (!validHex(a, 64) || !validHex(b, 64)) return false;
  let difference = 0;
  for (let i = 0; i < 64; i++) difference |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return difference === 0;
}
export const backgroundPoll = (method, path) =>
  method === 'GET' && (path === '/api/status' || path.startsWith('/api/logs/') || path.startsWith('/api/history'));

export class MqttTransport {
  constructor(options, client = new MqttClient(options)) {
    this.id = options.id;
    this.password = options.servicePassword;
    this.client = client;
    this.logs = new RemoteLogs();
    this.seq = 0;
    this.lastActivity = Date.now();
    this.cid = randomHex(8);
    this.generation = 0;
    this.keyCache = null;
    this.pendingStatus = null;
    this.sessionNonce = null;
    this.status = { state: 'offline' };
    this.tail = Promise.resolve();
    this.onstatus = () => {};
    this.onresponse = () => {};
    client.onmessage = (topic, payload, retained) => {
      this.receive(topic, payload, retained).catch(() => this.active?.reject(new ApiError('parse')));
    };
    client.onclose = () => {
      this.generation++;
      this.pendingStatus = null;
      this.logs.setState('offline');
      this.status = { state: 'offline' };
      this.onstatus(this.status);
      this.active?.reject(new ApiError('network'));
    };
  }
  setPassword(password) {
    this.password = password;
    this.keyCache = null;
    this.generation++;
    if (this.pendingStatus) this.acceptStatus(this.pendingStatus).catch(() => {});
  }
  keys(password, iterations) {
    if (!this.keyCache || this.keyCache.password !== password || this.keyCache.iterations !== iterations) {
      this.keyCache = { password, iterations, promise: deriveKeys(password, this.id, iterations) };
    }
    return this.keyCache.promise;
  }
  async acceptStatus(r) {
    const generation = ++this.generation;
    if (r.state === 'offline') {
      this.pendingStatus = null;
      this.status = { state: 'offline', verified: false };
      this.onstatus(this.status);
      return;
    }
    if (r.state !== 'online') return;
    this.pendingStatus = r;
    const available = { state: 'online', verified: false };
    if (
      r.v !== 2 ||
      !validHex(r.nonce, 32) ||
      !validHex(r.mac, 64) ||
      !Number.isInteger(r.kdf_iter) ||
      r.kdf_iter < 1000 ||
      r.kdf_iter > 100000 ||
      typeof r.fw !== 'string' ||
      typeof r.name !== 'string' ||
      ![r.until, r.remaining_s, r.budget_left_s].every((n) => Number.isSafeInteger(n) && n >= 0) ||
      (r.op != null &&
        (typeof r.op !== 'object' ||
          Array.isArray(r.op) ||
          !['mid', 'fw', 'result'].every((key) => typeof r.op[key] === 'string')))
    ) {
      this.status = available;
      this.onstatus(this.status);
      return;
    }
    if (!this.keyCache || this.keyCache.password !== this.password || this.keyCache.iterations !== r.kdf_iter) {
      this.status = { state: 'deriving', verified: false };
      this.onstatus(this.status);
    }
    const keys = await this.keys(this.password, r.kdf_iter);
    const expected = await hmac(keys.mac, statusMacText(this.id, r));
    if (generation !== this.generation) return;
    if (!equalMac(expected, r.mac)) {
      this.status = available;
      this.onstatus(this.status);
      return;
    }
    if (r.nonce !== this.sessionNonce) this.seq = 0;
    this.sessionNonce = r.nonce;
    this.status = { ...r, verified: true };
    this.onstatus(this.status);
    const a = this.active;
    if (a?.restart && r.nonce !== a.nonce && r.op?.mid === a.mid) this.completeRestart(a);
  }
  async connect() {
    await this.client.connect();
    await this.client.subscribe('gm/' + this.id + '/res/#');
    await this.client.subscribe('gm/' + this.id + '/status');
    this.logs.setState('subscribing');
    try {
      await this.client.subscribe('gm/' + this.id + '/log', 0);
      if (this.logs.state === 'offline' || (this.client.ws && this.client.ws.readyState !== 1))
        throw new Error('network');
      this.logs.setState('ready');
    } catch (e) {
      if (
        e.message === 'network' ||
        this.logs.state === 'offline' ||
        (this.client.ws && this.client.ws.readyState !== 1)
      ) {
        this.logs.setState('offline');
        throw new Error('network', { cause: e });
      }
      this.logs.setState(e.message === 'denied' ? 'denied' : 'error');
    }
  }
  async receive(topic, payload, retained) {
    if (topic === 'gm/' + this.id + '/log') {
      if (!retained) this.logs.append(payload);
      return;
    }
    if (encoder.encode(payload).length > 20 * 1024) throw new ApiError('parse');
    const r = JSON.parse(payload);
    if (!r || typeof r !== 'object' || Array.isArray(r)) throw new ApiError('parse');
    if (topic === 'gm/' + this.id + '/status') {
      await this.acceptStatus(r);
      return;
    }
    const a = this.active;
    if (!a || retained || topic !== 'gm/' + this.id + '/res/' + a.mid || a.handling || a.restart) return;
    a.handling = true;
    try {
      if (r.v !== 2 || r.seq !== a.seq || !Number.isInteger(r.status) || typeof r.body !== 'string')
        throw new ApiError('parse');
      if (r.mac === '') {
        let code = '';
        try {
          code = JSON.parse(r.body).error;
        } catch {
          /* Untrusted payload. */
        }
        if (!['bad_request', 'bad_mac', 'no_password', 'locked', 'busy'].includes(code)) code = 'untrusted';
        throw new ApiError('untrusted', 0, code);
      }
      if (
        r.cid !== a.cid ||
        !validHex(r.iv, 32) ||
        typeof r.ctype !== 'string' ||
        (r.restart !== undefined && typeof r.restart !== 'boolean')
      )
        throw new ApiError('parse');
      const expected = await hmac(a.keys.mac, macText('res', { ...r, ...a, id: this.id }));
      if (!equalMac(expected, r.mac)) throw new ApiError('parse');
      if (r.enc !== undefined && r.enc !== 'base64') throw new ApiError('parse');
      const success = r.status >= 200 && r.status < 300;
      const bytes = await cryptBody(a.keys.enc, { ...a, id: this.id, iv: r.iv }, 'res', unbase64(r.body));
      let data;
      if (success && r.enc === 'base64') {
        data = bytes.buffer;
      } else data = JSON.parse(decoder.decode(bytes));
      if (r.status < 200 || r.status >= 300) {
        const error = new ApiError('http', r.status, typeof data?.error === 'string' ? data.error : '');
        if (r.status === 409 && data?.error === 'offset') error.next = data.next;
        if (
          r.status === 409 &&
          data?.error === 'stale_seq' &&
          Number.isSafeInteger(data.last_seq) &&
          data.last_seq >= 0
        )
          error.lastSeq = data.last_seq;
        throw error;
      }
      if (r.restart === true) {
        a.restart = true;
        a.data = data;
        if (
          this.status.verified &&
          this.status.state === 'online' &&
          this.status.nonce !== a.nonce &&
          this.status.op?.mid === a.mid
        )
          this.completeRestart(a);
      } else a.resolve(data);
    } catch (e) {
      a.reject(e instanceof ApiError ? e : new ApiError('parse'));
    }
  }
  completeRestart(a) {
    if (this.status.op.result !== 'ok') a.reject(new ApiError('http', 422, this.status.op.result || 'restart'));
    else a.resolve(a.data);
  }
  async recover(signal) {
    if (signal?.aborted) throw new ApiError('aborted');
    if (this.client.ws && this.client.ws.readyState !== 1) {
      const abort = () => this.client.close();
      signal?.addEventListener('abort', abort, { once: true });
      try {
        await this.connect();
      } catch (e) {
        if (signal?.aborted) throw new ApiError('aborted');
        if (e.message === 'network' || e.message === 'timeout') throw new ApiError(e.message);
        throw e;
      } finally {
        signal?.removeEventListener('abort', abort);
      }
    }
    if (signal?.aborted) throw new ApiError('aborted');
    if (this.status.state === 'online' && this.status.verified) return;
    await new Promise((resolve, reject) => {
      const cleanup = () => {
        clearInterval(poll);
        clearTimeout(timer);
        signal?.removeEventListener('abort', abort);
      };
      const abort = () => {
        cleanup();
        reject(new ApiError('aborted'));
      };
      const poll = setInterval(() => {
        if (this.status.state === 'online' && this.status.verified) {
          cleanup();
          resolve();
        }
      }, 250);
      const timer = setTimeout(() => {
        cleanup();
        reject(new ApiError('timeout'));
      }, 60000);
      signal?.addEventListener('abort', abort, { once: true });
      if (signal?.aborted) abort();
    });
  }
  request(method, path, options = {}) {
    const run = () => this.perform(method, path, options);
    const result = this.tail.then(run);
    this.tail = result.catch(() => {});
    return result;
  }
  async perform(method, path, options) {
    if (options.signal?.aborted) throw new ApiError('aborted');
    if (options.reconnect) await this.recover(options.signal);
    if (this.status.state !== 'online' || !this.status.verified) throw new ApiError('network');
    if (!/^\/api\/[^?\n]*$/.test(path)) throw new ApiError('parse');
    const form = new URLSearchParams();
    for (const [k, v] of Object.entries(options.form || {}))
      if (v !== undefined && v !== null) form.append(k, String(v));
    const body = method === 'GET' ? '' : form.toString();
    if (encoder.encode(body).length > 2048) throw new ApiError('http', 413, 'too_large');
    // Signed sequence conflicts and retired clients get only bounded retries.
    let aligned = 0;
    let retired = 0;
    for (let busy = 0; ;) {
      try {
        const data = await this.exchange(method, path, body, options);
        // Mirrors the device: automatic reads do not count as activity for
        // the remote idle timeout (core/app/portal_tunnel.cpp backgroundPoll).
        if (!backgroundPoll(method, path)) this.lastActivity = Date.now();
        this.onresponse(data, path);
        if (method === 'POST' && path === '/api/service/exit') {
          this.status = { state: 'offline' };
          this.onstatus(this.status);
        }
        return data;
      } catch (e) {
        if (e.kind === 'http' && e.status === 409 && e.serverMessage === 'client_retired') {
          this.cid = randomHex(8);
          this.seq = 0;
          // A retired client may already have executed a mutation whose reply was lost.
          if (method === 'GET' && retired++ < 3) continue;
          throw e;
        }
        if (aligned < 3 && e.lastSeq !== undefined) {
          aligned++;
          this.seq = e.lastSeq;
          continue;
        }
        // The device is answering another client (e.g. a second tab): back off.
        if (e.serverMessage === 'busy' && busy < 3 && !options.signal?.aborted) {
          await new Promise((r) => setTimeout(r, 400 * ++busy));
          continue;
        }
        throw e;
      }
    }
  }
  async exchange(method, path, body, options) {
    if (!this.status.verified || this.status.state !== 'online') throw new ApiError('network');
    if (!Number.isSafeInteger(this.seq + 1)) throw new ApiError('parse');
    const generation = this.generation;
    const a = { mid: randomHex(8), cid: this.cid, seq: ++this.seq, nonce: this.status.nonce };
    a.keys = await this.keys(options.password ?? this.password, this.status.kdf_iter);
    const iv = randomHex(16);
    const encrypted = base64(await cryptBody(a.keys.enc, { ...a, id: this.id, iv }, 'req', encoder.encode(body)));
    const mac = await hmac(a.keys.mac, macText('req', { ...a, id: this.id, method, path, iv, body: encrypted }));
    if (generation !== this.generation) throw new ApiError('network');
    const payload = JSON.stringify({ v: 2, cid: a.cid, seq: a.seq, method, path, iv, body: encrypted, mac });
    if (encoder.encode(payload).length > 4096) throw new ApiError('http', 413, 'too_large');
    return new Promise((resolve, reject) => {
      const cleanup = () => {
        clearTimeout(timer);
        options.signal?.removeEventListener('abort', abort);
        if (this.active === a) this.active = null;
      };
      a.resolve = (data) => {
        cleanup();
        resolve(data);
      };
      a.reject = (e) => {
        cleanup();
        reject(e);
      };
      const abort = () => a.reject(new ApiError('aborted'));
      const timeout =
        path.startsWith('/api/update/') ||
        path === '/api/upload' ||
        path === '/api/ota/begin' ||
        path === '/api/ota/finish'
          ? 60000
          : 15000;
      const timer = setTimeout(
        () => a.reject(new ApiError('timeout')),
        options.timeoutOverride ?? options.timeout ?? timeout,
      );
      this.active = a;
      options.signal?.addEventListener('abort', abort, { once: true });
      if (options.signal?.aborted) {
        abort();
        return;
      }
      this.client.publish('gm/' + this.id + '/req/' + a.mid, payload).catch(() => a.reject(new ApiError('network')));
    });
  }
  close() {
    this.generation++;
    this.pendingStatus = null;
    this.active?.reject(new ApiError('aborted'));
    this.client.close();
  }
}
