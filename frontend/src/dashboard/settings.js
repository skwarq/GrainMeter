// Dashboard settings live only in this browser (localStorage). Nothing is
// sent anywhere except the read keys to api.thingspeak.com.
const KEY = 'gm.dashboard.v1';

export const DEFAULTS = {
  channelId: '',
  readKey: '',
  diagChannelId: '',
  diagReadKey: '',
  maxTemp: 25, // degC: grain above this needs attention
  risePerDay: 3, // degC per 24 h: typical sign of self-heating
  staleMinutes: 150, // device uploads hourly: > 2.5 h without data = problem
  range: '7d',
};

export function loadSettings() {
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return null;
    const s = JSON.parse(raw);
    return s && typeof s === 'object' && s.channelId ? { ...DEFAULTS, ...s } : null;
  } catch {
    return null;
  }
}

export function saveSettings(s) {
  try {
    localStorage.setItem(KEY, JSON.stringify(s));
    return true;
  } catch {
    return false;
  }
}

export function clearSettings() {
  try {
    localStorage.removeItem(KEY);
  } catch {
    /* ignore */
  }
}

/** Validation of the setup form; returns a message key or ''. */
export function validateSettings(s) {
  if (!/^\d{1,9}$/.test(String(s.channelId).trim())) return 'dash.errChannel';
  if (s.readKey && !/^[A-Za-z0-9]{8,32}$/.test(s.readKey.trim())) return 'dash.errKey';
  if (s.diagChannelId && !/^\d{1,9}$/.test(String(s.diagChannelId).trim())) return 'dash.errChannel';
  if (s.diagReadKey && !/^[A-Za-z0-9]{8,32}$/.test(s.diagReadKey.trim())) return 'dash.errKey';
  for (const k of ['maxTemp', 'risePerDay', 'staleMinutes']) {
    if (s[k] !== '' && s[k] !== null && !Number.isFinite(Number(s[k]))) return 'dash.errNumber';
  }
  return '';
}
