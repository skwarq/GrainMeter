// Client-side pre-check of a firmware image (header only). The device does the
// real validation; this only gives an early, friendlier hint.

export const ESP_IMAGE_MAGIC = 0xe9;
export const ESP_CHIP_ID_C6 = 0x000d;
export const APP_DESC_MAGIC = 0xabcd5432;

function cstr(bytes, off, len) {
  let s = '';
  for (let i = off; i < off + len && i < bytes.length; i++) {
    if (bytes[i] === 0) break;
    s += String.fromCharCode(bytes[i]);
  }
  return s;
}

/**
 * @param {Uint8Array} b first >= 112 bytes of the file
 * @returns {{ok:boolean, reason?:string, version?:string, project?:string}}
 */
export function inspectImage(b) {
  if (!b || b.length < 112) return { ok: false, reason: 'short' };
  if (b[0] !== ESP_IMAGE_MAGIC) return { ok: false, reason: 'magic' };
  const chip = b[12] | (b[13] << 8);
  if (chip !== ESP_CHIP_ID_C6) return { ok: false, reason: 'chip' };
  const magic = (b[32] | (b[33] << 8) | (b[34] << 16) | (b[35] << 24)) >>> 0;
  if (magic !== APP_DESC_MAGIC) return { ok: false, reason: 'desc' };
  const version = cstr(b, 48, 32);
  const project = cstr(b, 80, 32);
  return { ok: project === 'grainmeter', reason: project === 'grainmeter' ? undefined : 'project', version, project };
}
