import { ApiError } from '../lib/errors.js';
const DEFAULT_TIMEOUT_MS = 8000;
const SLOW_TIMEOUT_MS = 30000;
const OTA_TIMEOUT_MS = 10 * 60 * 1000;
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
export async function request(method, path, opt = {}) {
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
      const error = new ApiError('http', res.status, msg);
      if (res.status === 409 && msg === 'offset') error.next = data.next;
      throw error;
    }
    if (data === undefined || data === null) throw new ApiError('parse', res.status);
    return data;
  } finally {
    clearTimeout(timer);
    if (opt.signal) opt.signal.removeEventListener('abort', onOuterAbort);
  }
}

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

export function uploadFirmware(file, onProgress, ensureAuth, withAuth) {
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
