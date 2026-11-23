// Message lookup without any UI dependency (used by components and tests).
import pl from './pl.js';
import en from './en.js';

export const MESSAGES = { pl, en };
export const LANGS = ['pl', 'en'];
export const DEFAULT_LANG = 'pl';

/** Looks up `key` for `lang` (falls back to Polish, then the key) and fills {placeholders}. */
export function translate(lang, key, params) {
  const table = MESSAGES[lang] || MESSAGES[DEFAULT_LANG];
  let s = table[key];
  if (s === undefined) s = MESSAGES[DEFAULT_LANG][key];
  if (s === undefined) return key;
  if (params) {
    s = s.replace(/\{(\w+)\}/g, (m, name) => (params[name] === undefined ? m : String(params[name])));
  }
  return s;
}
