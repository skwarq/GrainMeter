import { createContext } from 'preact';
import { useContext } from 'preact/hooks';
import { translate, DEFAULT_LANG } from '../i18n/translate.js';

export const I18nContext = createContext({
  lang: DEFAULT_LANG,
  t: (key, params) => translate(DEFAULT_LANG, key, params),
  setLang: () => {},
});

export const useI18n = () => useContext(I18nContext);

/** Human readable text for an ApiError (uses lib/errors.js mapping). */
export function errorText(t, described) {
  if (!described.key) return described.detail;
  const base = t(described.key, described.params);
  return described.detail ? `${base} (${described.detail})` : base;
}

/** Translation of `prefix + value` or the raw device value when there is none. */
export function tOr(t, prefix, value) {
  if (value === null || value === undefined || value === '') return '—';
  const key = prefix + value;
  const s = t(key);
  return s === key ? String(value) : s;
}
