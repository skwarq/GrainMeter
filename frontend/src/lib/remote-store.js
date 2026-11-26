// Remote-access settings shared by the dashboard (index.html) and the remote
// service page (remote.html): both live on the same origin, so one entry in
// this browser's localStorage serves both pages. Passwords (broker, service)
// are stored only when the user ticks "remember"; the rest always is.
export const REMOTE_KEY = 'gm.remote';
export const REMOTE_FIELDS = [
  'url',
  'username',
  'password',
  'id',
  'servicePassword',
  'talkbackId',
  'talkbackKey',
  'minutes',
];
export const REMOTE_SECRETS = ['password', 'servicePassword'];

export function loadRemote(storage = globalThis.localStorage) {
  try {
    const v = JSON.parse(storage.getItem(REMOTE_KEY));
    if (!v || typeof v !== 'object') return { form: {}, remembered: false };
    const form = Object.fromEntries(REMOTE_FIELDS.filter((k) => typeof v[k] === 'string').map((k) => [k, v[k]]));
    return { form, remembered: REMOTE_SECRETS.some((k) => form[k]) };
  } catch {
    return { form: {}, remembered: false };
  }
}

/** Saves the form; secrets only with remember=true. Throws if storage fails. */
export function saveRemote(form, remember, storage = globalThis.localStorage) {
  const out = {};
  for (const k of REMOTE_FIELDS) {
    const v = form[k];
    if (typeof v !== 'string' || v === '') continue;
    if (REMOTE_SECRETS.includes(k) && !remember) continue;
    out[k] = v;
  }
  if (Object.keys(out).length) storage.setItem(REMOTE_KEY, JSON.stringify(out));
  else storage.removeItem(REMOTE_KEY);
}
