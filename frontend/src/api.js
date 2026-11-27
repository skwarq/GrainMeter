// Device API facade; transport implementations own network I/O.
// - JSON responses, errors normalised to ApiError (see lib/errors.js)
// - timeouts via AbortController
// - service password: asked on the first write action, verified with
//   POST /api/login, kept in sessionStorage, re-asked after 401.

import * as http from './transport/http.js';
import { ApiError } from './lib/errors.js';
import { rememberFromConfig, rememberFromStatus } from './lib/device-schedule.js';

const PW_KEY = 'gm.svcpw';
const SLOW_TIMEOUT_MS = 30000; // scan / measure / upload window can take a while

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
  return remote ? !!remote.password : !!readPw();
}

export function forgetPassword() {
  if (remote) {
    remote.setPassword('');
    remote.close();
    return;
  }
  writePw(null);
}

let remote = null;
export function setRemoteTransport(transport) {
  remote = transport;
}
export const isRemote = () => !!remote;
/** Live MQTT log feed of the remote transport (gm/<id>/log), if any. */
export const remoteLogModel = () => (remote && remote.logs) || null;
/** Time (ms) of the last user action sent over the tunnel, or null. */
export const remoteActivity = () => (remote ? remote.lastActivity : null);
function request(method, path, opt) {
  return (remote || http).request(method, path, opt);
}

/** Checks a password with POST /api/login; stores it on success. */
export async function login(password) {
  const r = await request('POST', '/api/login', { password });
  if (remote) remote.setPassword(password);
  else writePw(password);
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
  if (remote) return fn(undefined);
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

function post(path, form, timeout, signal, reconnect) {
  return withAuth((pw) => request('POST', path, { form, password: pw, timeout, signal, reconnect }));
}

// ---- read-only -------------------------------------------------------------

// Over the remote portal (same origin as the dashboard) the schedule is
// remembered, so the dashboard can predict the next wake-up window.
const remember = (fn) => (r) => {
  if (remote) fn(r);
  return r;
};
export const getStatus = (signal) => request('GET', '/api/status', { signal }).then(remember(rememberFromStatus));
export const getDiag = (signal) => request('GET', '/api/diag', { signal });
export const getLogs = (since, signal) => request('GET', `/api/logs/${since}`, { signal });
export const getConfig = (signal) => request('GET', '/api/config', { signal }).then(remember(rememberFromConfig));
export const getOta = (signal) => request('GET', '/api/ota', { signal, reconnect: true });
export const otaBegin = (form, signal) => post('/api/ota/begin', form, 60000, signal);
export const otaChunk = (form, signal) => post('/api/ota/chunk', form, undefined, signal);
export const otaFinish = (form, signal) => post('/api/ota/finish', form, 60000, signal);
export const otaAbort = () => post('/api/ota/abort', null, 60000, undefined, true);

/** Local history summary: records, used/capacity bytes, oldest/newest... */
export const getHistoryInfo = (signal) => request('GET', '/api/history', { signal });

/** One page of the local history (epoch seconds, inclusive) as an
 *  ArrayBuffer for lib/feed.js parseHistoryBinary; continue from its `next`. */
export function getHistoryPage(from, to, signal) {
  if (remote) return request('GET', `/api/history/${Math.floor(from)}/${Math.floor(to)}`, { signal });
  return http.getHistoryPage(from, to, signal);
}

// ---- actions (password) ----------------------------------------------------

export const scan = () => post('/api/scan', null, SLOW_TIMEOUT_MS);
/** Nearby Wi-Fi networks: {networks:[{ssid, rssi, channel, secure}]}, strongest first. */
export const wifiScan = () => post('/api/wifi/scan', null, SLOW_TIMEOUT_MS);
/** Starts a connection attempt (wifi_ssid, wifi_pass, static IP fields); poll getWifiJoin. */
export const wifiJoin = (form) => post('/api/wifi/join', form);
/** {state: idle|pending|running|ok|failed, ssid, message, ip, rssi} */
export const getWifiJoin = (signal) => request('GET', '/api/wifi/join', { signal, timeout: 4000 });
export const measure = (store) => post('/api/measure', store ? { store: 1 } : null, SLOW_TIMEOUT_MS);
export const notifyTest = () => post('/api/notify/test', null, 90000);
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
export const extendRemote = (minutes) => post('/api/remote/extend', { minutes });
export const simAction = (args) => post('/api/sim', args, SLOW_TIMEOUT_MS);
/** Erases the stored crash record (core dump) on the ESP32. */
export const clearCrash = () => post('/api/crash/clear');

/** Saves configuration; on success the new service password (if any) replaces the stored one. */
export async function saveConfig(form) {
  const r = await post('/api/config', form);
  if (form.svc_pass) {
    if (remote) remote.setPassword(form.svc_pass);
    else writePw(form.svc_pass);
  }
  return r;
}

/**
 * Firmware upload as multipart/form-data with progress.
 * @param {File} file
 * @param {(sent:number,total:number)=>void} onProgress
 * @returns {{promise: Promise<any>, abort: () => void}}
 */
export function uploadFirmware(file, onProgress) {
  if (remote) return { promise: Promise.reject(new ApiError('unsupported')), abort() {} };
  return http.uploadFirmware(file, onProgress, ensureAuth, withAuth);
}
