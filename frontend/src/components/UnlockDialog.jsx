import { useEffect, useRef, useState } from 'preact/hooks';
import { login } from '../api.js';
import { describeError } from '../lib/errors.js';
import { useI18n, errorText } from './i18n.js';
import { Button, Icon } from './ui.jsx';

let failures = 0;

/** Asks for the service password and verifies it with POST /api/login. */
export function UnlockDialog({ request, onDone }) {
  const { t } = useI18n();
  const [pw, setPw] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState(request.message ? t('unlock.rejected', { msg: request.message }) : '');
  const input = useRef(null);

  useEffect(() => {
    if (input.current) input.current.focus();
    const onKey = (e) => {
      if (e.key === 'Escape') onDone(null);
    };
    document.addEventListener('keydown', onKey);
    return () => document.removeEventListener('keydown', onKey);
  }, [onDone]);

  const submit = async (e) => {
    e.preventDefault();
    if (!pw) return;
    setBusy(true);
    setError('');
    try {
      await login(pw);
      failures = 0;
      onDone(pw);
    } catch (err) {
      setBusy(false);
      if (err.kind === 'http' && err.status === 401) {
        failures++;
        let msg = t('unlock.wrong', { msg: err.serverMessage || '401' });
        if (failures >= 5) msg += ' ' + t('unlock.lockout');
        setError(msg);
        setPw('');
        if (input.current) input.current.focus();
      } else {
        setError(errorText(t, describeError(err)));
      }
    }
  };

  return (
    <div class="overlay" role="presentation" onClick={(e) => e.target === e.currentTarget && onDone(null)}>
      <form class="dialog" role="dialog" aria-modal="true" aria-labelledby="unlock-title" onSubmit={submit}>
        <div class="dialog-icon">
          <Icon name="lock" size={26} />
        </div>
        <h2 id="unlock-title">{t('unlock.title')}</h2>
        <p class="muted">{t('unlock.text')}</p>
        <label class="field">
          <span>{t('unlock.password')}</span>
          <input
            ref={input}
            type="password"
            autocomplete="current-password"
            value={pw}
            onInput={(e) => setPw(e.currentTarget.value)}
            disabled={busy}
          />
        </label>
        {error && (
          <p class="result result-error" role="alert">
            <Icon name="warn" size={16} />
            <span>{error}</span>
          </p>
        )}
        <div class="dialog-actions">
          <Button onClick={() => onDone(null)}>{t('common.cancel')}</Button>
          <Button type="submit" variant="primary" busy={busy} disabled={!pw}>
            {t('unlock.submit')}
          </Button>
        </div>
      </form>
    </div>
  );
}
