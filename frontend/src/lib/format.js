// Small formatting helpers (pure, no DOM).

/** Splits a duration into the two most significant units. */
export function durationParts(sec) {
  if (typeof sec !== 'number' || !Number.isFinite(sec) || sec < 0) return null;
  const s = Math.floor(sec);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const r = s % 60;
  const two = (a, b, bv) => (bv > 0 ? [a, b] : [a]);
  if (d > 0) return two(d + ' d', h + ' h', h);
  if (h > 0) return two(h + ' h', m + ' min', m);
  if (m > 0) return two(m + ' min', r + ' s', r);
  return [r + ' s'];
}

/** "1 h 5 min" / "42 s"; `missing` for invalid input. */
export function formatDuration(sec, missing = '—') {
  const p = durationParts(sec);
  return p ? p.join(' ') : missing;
}

/** "mm:ss" or "h:mm:ss" countdown. */
export function formatClock(sec) {
  const s = Math.max(0, Math.floor(sec || 0));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const r = s % 60;
  const mm = String(m).padStart(2, '0');
  const ss = String(r).padStart(2, '0');
  return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
}

export function formatBytes(n) {
  if (typeof n !== 'number' || !Number.isFinite(n)) return '—';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KiB';
  return (n / 1024 / 1024).toFixed(2) + ' MiB';
}

/** "28 FF 64 85 1E 16 03 04" -> grouped for reading; input kept if not 16 hex chars. */
export function formatRom(rom) {
  if (typeof rom !== 'string') return '—';
  if (!/^[0-9A-Fa-f]{16}$/.test(rom)) return rom;
  return rom.toUpperCase().match(/../g).join(' ');
}

/** Family code from a ROM string ("28"), or null. */
export function romFamily(rom) {
  return typeof rom === 'string' && /^[0-9A-Fa-f]{16}$/.test(rom) ? rom.slice(0, 2).toUpperCase() : null;
}

/** Local date/time from a UTC epoch (seconds); null for unknown (0). */
export function formatEpoch(epoch, locale) {
  if (typeof epoch !== 'number' || epoch <= 0) return null;
  try {
    return new Date(epoch * 1000).toLocaleString(locale);
  } catch {
    return new Date(epoch * 1000).toISOString();
  }
}

/** RSSI (dBm) -> 0..4 bars; 0 means no signal / unknown. */
export function rssiBars(rssi) {
  if (typeof rssi !== 'number' || rssi >= 0) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  if (rssi >= -90) return 1;
  return 0;
}

/** Number of samples per upload as computed by the firmware. */
export function samplesPerUpload(measureS, uploadS) {
  const m = Number(measureS);
  const u = Number(uploadS);
  if (!Number.isFinite(m) || !Number.isFinite(u) || m <= 0 || u <= 0) return null;
  return Math.max(1, Math.floor(u / m));
}
