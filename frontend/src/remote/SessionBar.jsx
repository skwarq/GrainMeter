import { useEffect, useState } from 'preact/hooks';
import * as api from '../api.js';
import { translate } from '../i18n/translate.js';
import { describeError } from '../lib/errors.js';
import { errorText } from '../components/i18n.js';
import { remainingSeconds, sessionTime } from './session.js';

export function SessionBar({ online, session, ended, lang }) {
  const [now, setNow] = useState(Date.now);
  const [minutes, setMinutes] = useState('10');
  const [ask, setAsk] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [capped, setCapped] = useState(false);
  const t = (key, params) => translate(lang, key, params);
  useEffect(() => {
    setNow(Date.now());
    if (!online) {
      setAsk(false);
      return;
    }
    const timer = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(timer);
  }, [online, session]);
  const run = async (action) => {
    setBusy(true);
    setError('');
    try {
      await action();
      setAsk(false);
    } catch (e) {
      setError(errorText(t, describeError(e)));
    } finally {
      setBusy(false);
    }
  };
  // The daily budget is a battery safety limit: shown only when it matters
  // (under 10 min left, or an extension it cut short).
  const extend = () =>
    run(async () => {
      const before = remainingSeconds(session, Date.now()) || 0;
      const r = await api.extendRemote(n);
      setCapped(Number.isFinite(r?.remaining_s) && r.remaining_s < Math.min(before + n * 60, 3600) - 30);
    });
  const lowBudget = session?.budgetLeft != null && session.budgetLeft < 600;
  // The device ends an idle session after remote_idle_min without a user
  // action (automatic status/log reads do not count): warn 2 min before.
  const [idleMin, setIdleMin] = useState(10);
  useEffect(() => {
    if (online)
      api.getConfig().then(
        (c) => Number(c.remote_idle_min) > 0 && setIdleMin(Number(c.remote_idle_min)),
        () => {},
      );
  }, [online]);
  const idleLeft = Math.ceil((idleMin * 60 * 1000 - (now - (api.remoteActivity() ?? now))) / 1000);
  const idleSoon = online && idleLeft <= 120;
  const n = Number(minutes);
  const valid = Number.isInteger(n) && n >= 1 && n <= 60;
  if (!online && !ended && !error) return null;
  return (
    <div class="remote-session">
      {online && (
        <>
          <p role="status">
            {t('connection.remaining')}: <strong>{sessionTime(remainingSeconds(session, now))}</strong>
            {lowBudget && ' · ' + t('connection.budgetLeft') + ': ' + sessionTime(session?.budgetLeft)}
          </p>
          <div class="row-actions">
            <label>
              {t('connection.extendMinutes')}
              <input
                type="number"
                min="1"
                max="60"
                value={minutes}
                onInput={(e) => setMinutes(e.currentTarget.value)}
              />
            </label>
            <button type="button" disabled={busy || !valid} onClick={extend}>
              {t('connection.extend', { n: minutes })}
            </button>
            <button type="button" disabled={busy} onClick={() => setAsk(true)}>
              {t('connection.end')}
            </button>
          </div>
          {ask && (
            <div class="confirm" role="alertdialog">
              <p>{t('connection.endConfirm')}</p>
              <button type="button" disabled={busy} onClick={() => setAsk(false)}>
                {t('common.cancel')}
              </button>
              <button type="button" disabled={busy} onClick={() => run(() => api.exitService())}>
                {t('common.confirm')}
              </button>
            </div>
          )}
        </>
      )}
      {online && capped && <p role="status">{t('connection.capped')}</p>}
      {idleSoon && <p role="alert">{t('connection.idleSoon', { s: Math.max(0, idleLeft) })}</p>}
      {ended && <p role="status">{t('connection.ended')}</p>}
      {error && <p role="alert">{error}</p>}
    </div>
  );
}
