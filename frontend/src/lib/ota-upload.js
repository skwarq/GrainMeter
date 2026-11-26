import { ApiError, isOffline } from './errors.js';
import { ESP_IMAGE_MAGIC } from './ota.js';

const ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';

function base64url(bytes) {
  let text = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const n = (bytes[i] << 16) | ((bytes[i + 1] || 0) << 8) | (bytes[i + 2] || 0);
    text += ALPHABET[n >>> 18] + ALPHABET[(n >>> 12) & 63];
    if (i + 1 < bytes.length) text += ALPHABET[(n >>> 6) & 63];
    if (i + 2 < bytes.length) text += ALPHABET[n & 63];
  }
  return text;
}

/** Chunked OTA over either transport. Progress counts acknowledged bytes. */
export async function uploadFirmware(file, api, { onProgress = () => {}, signal } = {}) {
  const checkAbort = () => {
    if (signal?.aborted) throw new ApiError('aborted');
  };
  const offset = (n) => {
    if (!Number.isSafeInteger(n) || n < 0 || n > file.size) throw new ApiError('parse');
    return n;
  };
  let begun = false;
  try {
    checkAbort();
    if (!file.size) throw new ApiError('http', 415, 'empty');
    const bytes = new Uint8Array(await file.arrayBuffer());
    checkAbort();
    if (bytes[0] !== ESP_IMAGE_MAGIC) throw new ApiError('http', 415, 'magic');
    const sha256 = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), (b) =>
      b.toString(16).padStart(2, '0'),
    ).join('');
    checkAbort();
    // Not cancellable while pending: only a begin the device accepted is ours
    // to abort (a rejected one, e.g. 409 busy, may be another tab's upload).
    const begin = await api.otaBegin({ size: file.size, sha256, name: file.name });
    begun = true;
    checkAbort();
    const chunk = begin.chunk ?? 1440;
    if (!Number.isSafeInteger(chunk) || chunk < 1 || chunk > 1440) throw new ApiError('parse');
    let off = offset(begin.next);
    let retries = 0;
    let recovering = false;
    const start = Date.now();
    const initial = off;
    const progress = (stage = 'uploading') => {
      const sent = off - initial;
      onProgress({
        sent: off,
        total: file.size,
        percent: (off / file.size) * 100,
        eta: sent > 0 ? Math.max(0, ((Date.now() - start) / 1000) * ((file.size - off) / sent)) : null,
        stage,
      });
    };
    progress();
    while (off < file.size) {
      checkAbort();
      let next;
      try {
        if (recovering) {
          const state = await api.getOta(signal);
          if (state.expected !== file.size) throw new ApiError('http', 409, 'ota_session');
          next = offset(state.received);
          recovering = false;
        } else {
          const end = Math.min(off + chunk, file.size);
          const r = await api.otaChunk({ off, d: base64url(bytes.subarray(off, end)) }, signal);
          next = offset(r.next);
          if (next < end) throw new ApiError('parse');
        }
      } catch (e) {
        checkAbort();
        if (e.kind === 'http' && e.status === 409 && e.serverMessage === 'offset') {
          next = offset(e.next);
          if (++retries > 3) throw e;
        } else if (isOffline(e) && retries++ < 3) {
          // A timeout may just be a lost reply: resend the identical chunk.
          // A broken link needs the device's authoritative offset first.
          recovering = recovering || e.kind === 'network';
          continue;
        } else throw e;
      }
      if (next > off) retries = 0;
      off = next;
      progress();
    }
    checkAbort();
    progress('finishing');
    return await api.otaFinish({ sha256 }, signal);
  } catch (e) {
    if (begun && (signal?.aborted || e.kind === 'aborted')) {
      // Do not pass the cancelled signal: cleanup must reach the device.
      await api.otaAbort();
      throw new ApiError('aborted');
    }
    throw e;
  }
}
