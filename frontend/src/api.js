// The ONLY module that talks to the device (fetch / XMLHttpRequest).
// - JSON responses, errors normalised to ApiError (see lib/errors.js)
// - timeouts via AbortController
// - service password: asked on the first write action, verified with
//   POST /api/login, kept in sessionStorage, re-asked after 401.

import { ApiError } from './lib/errors.js';

const PW_KEY = 'gm.svcpw';
const DEFAULT_TIMEOUT_MS = 8000;
const SLOW_TIMEOUT_MS = 30000; // scan / measure / upload window can take a while
const OTA_TIMEOUT_MS = 10 * 60 * 1000;

let memoryPassword = null;
/** @type {null | ((info:{message:string}) => Promise<string|null>)} */
let passwordPrompt = null;

function readPw() {
  try {
    return sessionStorage.getItem(PW_KEY);
  } catch {
    return memoryPassword; // storage blocked: keep it for this page only
  }
}

function writePw(pw) {
  try {
    if (pw) sessionStorage.setItem(PW_KEY, pw);
    else sessionStorage.removeItem(PW_KEY);
  } catch {
    memoryPassword = pw || null;
  }
}

/** Registers the UI function that asks for (and verifies) the service password. */
export function setPasswordPrompt(fn) {
  passwordPrompt = fn;
}

export function hasPassword() {
  return !!readPw();
}

export function forgetPassword() {
  writePw(null);
}

function parseJson(text) {
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch {
    return undefined;
  }
}

function toForm(data) {
  if (!data) return undefined;
  const f = new URLSearchParams();
  for (const [k, v] of Object.entries(data)) {
    if (v === undefined || v === null) continue;
    f.append(k, String(v));
  }
  return f;
}

/**
 * @param {'GET'|'POST'} method
 * @param {string} path
 * @param {{form?:object, password?:string, timeout?:number, signal?:AbortSignal}} [opt]
 */
async function request(method, path, opt = {}) {
  const ctrl = new AbortController();
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    ctrl.abort();
  }, opt.timeout || DEFAULT_TIMEOUT_MS);
  const onOuterAbort = () => ctrl.abort();
  if (opt.signal) {
    if (opt.signal.aborted) ctrl.abort();
    else opt.signal.addEventListener('abort', onOuterAbort, { once: true });
  }
  const headers = { Accept: 'application/json' };
  if (opt.password !== undefined) headers['X-GM-Password'] = opt.password;
  try {
    let res;
    let text;
    try {
      res = await fetch(path, {
        method,
        headers,
        body: method === 'POST' ? toForm(opt.form) || '' : undefined,
        cache: 'no-store',
        credentials: 'same-origin',
        signal: ctrl.signal,
      });
      text = await res.text();
    } catch {
      if (timedOut) throw new ApiError('timeout');
      if (opt.signal && opt.signal.aborted) throw new ApiError('aborted');
      throw new ApiError('network');
    }
    const data = parseJson(text);
    if (!res.ok) {
      const msg = data && typeof data.error === 'string' ? data.error : '';
      throw new ApiError('http', res.status, msg);
    }
    if (data === undefined || data === null) throw new ApiError('parse', res.status);
    return data;
  } finally {
    clearTimeout(timer);
    if (opt.signal) opt.signal.removeEventListener('abort', onOuterAbort);
  }
}

/** Checks a password with POST /api/login; stores it on success. */
export async function login(password) {
  const r = await request('POST', '/api/login', { password });
  writePw(password);
  return r;
}

let pendingPrompt = null;

async function askPassword(message) {
  if (!passwordPrompt) throw new ApiError('aborted');
  // concurrent write actions share one dialog
  if (!pendingPrompt) {
    pendingPrompt = Promise.resolve(passwordPrompt({ message })).finally(() => (pendingPrompt = null));
  }
  const pw = await pendingPrompt;
  if (!pw) throw new ApiError('aborted');
  return pw;
}

/** Runs fn(password); asks for the password first and again after every 401. */
async function withAuth(fn) {
  let pw = readPw();
  let message = '';
  for (;;) {
    if (!pw) pw = await askPassword(message);
    try {
      return await fn(pw);
    } catch (e) {
      if (e instanceof ApiError && e.kind === 'http' && e.status === 401) {
        writePw(null);
        pw = null;
        message = e.serverMessage || 'unauthorized';
        continue;
      }
      throw e;
    }
  }
}

/** Makes sure a verified password is stored (opens the unlock dialog if not). */
export function ensureAuth() {
  return withAuth((pw) => request('POST', '/api/login', { password: pw }));
}

function post(path, form, timeout) {
  return withAuth((pw) => request('POST', path, { form, password: pw, timeout }));
}

// ---- read-only -------------------------------------------------------------

export const getStatus = (signal) => request('GET', '/api/status', { signal });
export const getDiag = (signal) => request('GET', '/api/diag', { signal });
export const getConfig = (signal) => request('GET', '/api/config', { signal });
export const getOta = (signal) => request('GET', '/api/ota', { signal });

/** Local history summary: records, used/capacity bytes, oldest/newest... */
export const getHistoryInfo = (signal) => request('GET', '/api/history', { signal });

/** One page of the local history (epoch seconds, inclusive) as an
 *  ArrayBuffer for lib/feed.js parseHistoryBinary; continue from its `next`. */
export async function getHistoryPage(from, to, signal) {
  const ctrl = new AbortController();
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    ctrl.abort();
  }, SLOW_TIMEOUT_MS);
  const onOuterAbort = () => ctrl.abort();
  if (signal) signal.addEventListener('abort', onOuterAbort, { once: true });
  try {
    let res;
    try {
      res = await fetch(`/api/history/${Math.floor(from)}/${Math.floor(to)}`, {
        cache: 'no-store',
        credentials: 'same-origin',
        signal: ctrl.signal,
      });
    } catch {
      if (timedOut) throw new ApiError('timeout');
      if (signal && signal.aborted) throw new ApiError('aborted');
      throw new ApiError('network');
    }
    if (!res.ok) {
      const data = parseJson(await res.text());
      throw new ApiError('http', res.status, data && typeof data.error === 'string' ? data.error : '');
    }
    return await res.arrayBuffer();
  } finally {
    clearTimeout(timer);
    if (signal) signal.removeEventListener('abort', onOuterAbort);
  }
}

// ---- actions (password) ----------------------------------------------------

export const scan = () => post('/api/scan', null, SLOW_TIMEOUT_MS);
export const measure = (store) => post('/api/measure', store ? { store: 1 } : null, SLOW_TIMEOUT_MS);
export const uploadNow = () => post('/api/upload', null, 90000);
/** Remote update: read the manifest now / download, verify and install. */
export const remoteUpdateCheck = () => post('/api/update/check', null, 60000);
export const remoteUpdateInstall = (version) => post('/api/update/install', { version }, 240000);
export const mapSensor = (rom, point, heightCm) =>
  post('/api/map', { rom, point, height_cm: heightCm === null || heightCm === '' ? undefined : heightCm });
export const unmapSensor = (rom) => post('/api/unmap', { rom });
export const automap = () => post('/api/automap');
export const restart = () => post('/api/restart');
export const exitService = () => post('/api/service/exit');
export const simAction = (args) => post('/api/sim', args, SLOW_TIMEOUT_MS);

/** Saves configuration; on success the new service password (if any) replaces the stored one. */
export async function saveConfig(form) {
  const r = await post('/api/config', form);
  if (form.svc_pass) writePw(form.svc_pass);
  return r;
}

/**
 * Firmware upload as multipart/form-data with progress.
 * @param {File} file
 * @param {(sent:number,total:number)=>void} onProgress
 * @returns {{promise: Promise<any>, abort: () => void}}
 */
export function uploadFirmware(file, onProgress) {
  let xhr = null;
  let aborted = false;
  const send = (pw) =>
    new Promise((resolve, reject) => {
      if (aborted) return reject(new ApiError('aborted'));
      xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota');
      xhr.setRequestHeader('X-GM-Password', pw);
      xhr.setRequestHeader('Accept', 'application/json');
      xhr.timeout = OTA_TIMEOUT_MS;
      xhr.upload.onprogress = (ev) => {
        if (ev.lengthComputable) onProgress(ev.loaded, ev.total);
      };
      xhr.onload = () => {
        const data = parseJson(xhr.responseText);
        if (xhr.status >= 200 && xhr.status < 300 && data) resolve(data);
        else if (xhr.status >= 200 && xhr.status < 300) reject(new ApiError('parse', xhr.status));
        else reject(new ApiError('http', xhr.status, data && typeof data.error === 'string' ? data.error : ''));
      };
      xhr.onerror = () => reject(new ApiError('network'));
      xhr.ontimeout = () => reject(new ApiError('timeout'));
      xhr.onabort = () => reject(new ApiError('aborted'));
      const fd = new FormData();
      fd.append('firmware', file, file.name || 'firmware.bin');
      xhr.send(fd);
    });
  // Verify the password first: a 401 sent while the browser is still pushing
  // a large body often surfaces only as a network error.
  const promise = ensureAuth().then(() => withAuth(send));
  return {
    promise,
    abort() {
      aborted = true;
      if (xhr) xhr.abort();
    },
  };
}
