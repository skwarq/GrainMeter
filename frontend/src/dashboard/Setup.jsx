import { useState } from 'preact/hooks';
import { useI18n } from '../components/i18n.js';
import { Card, Button, Logo } from '../components/ui.jsx';
import { DEFAULTS, validateSettings } from './settings.js';

function Field({ label, hint, children }) {
  return (
    <label class="field">
      <span>{label}</span>
      {children}
      {hint && <small class="hint">{hint}</small>}
    </label>
  );
}

/** First run and settings: channel IDs, read keys and alert thresholds. */
export function Setup({ initial, onSave, onCancel, onForget }) {
  const { t } = useI18n();
  const [f, setF] = useState({ ...DEFAULTS, ...(initial || {}) });
  const [err, setErr] = useState('');
  const set = (k) => (e) => setF({ ...f, [k]: e.currentTarget.value });

  const submit = (e) => {
    e.preventDefault();
    const clean = {
      ...f,
      channelId: String(f.channelId).trim(),
      readKey: f.readKey.trim(),
      diagChannelId: String(f.diagChannelId).trim(),
      diagReadKey: f.diagReadKey.trim(),
    };
    const v = validateSettings(clean);
    if (v) return setErr(t(v));
    onSave({
      ...clean,
      maxTemp: clean.maxTemp === '' ? null : Number(clean.maxTemp),
      risePerDay: clean.risePerDay === '' ? 0 : Number(clean.risePerDay),
      staleMinutes: clean.staleMinutes === '' ? 0 : Number(clean.staleMinutes),
    });
  };

  const key = (k) => ({
    type: 'password',
    autocomplete: 'off',
    autocapitalize: 'characters',
    spellcheck: false,
    value: f[k],
    onInput: set(k),
  });

  return (
    <form class="dash-setup" onSubmit={submit} autocomplete="off">
      <div class="dash-setup-head">
        <Logo size={40} />
        <div>
          <h1>{t('dash.title')}</h1>
          <p class="muted">{t('dash.setupSub')}</p>
        </div>
      </div>

      <Card title={t('dash.mainChannel')} icon="sensors">
        <div class="form-grid">
          <Field label="Channel ID" hint={t('dash.channelHint')}>
            <input type="text" inputMode="numeric" value={f.channelId} onInput={set('channelId')} required />
          </Field>
          <Field label="Read API Key" hint={t('dash.readKeyHint')}>
            <input {...key('readKey')} />
          </Field>
        </div>
      </Card>

      <Card title={t('dash.diagChannel')} icon="diag">
        <div class="form-grid">
          <Field label="Channel ID" hint={t('dash.optional')}>
            <input type="text" inputMode="numeric" value={f.diagChannelId} onInput={set('diagChannelId')} />
          </Field>
          <Field label="Read API Key" hint={t('dash.optional')}>
            <input {...key('diagReadKey')} />
          </Field>
        </div>
      </Card>

      <Card title={t('dash.alertsTitle')} icon="warn">
        <div class="form-grid">
          <Field label={t('dash.maxTemp')} hint={t('dash.maxTempHint')}>
            <input type="number" step="0.5" value={f.maxTemp ?? ''} onInput={set('maxTemp')} />
          </Field>
          <Field label={t('dash.risePerDay')} hint={t('dash.risePerDayHint')}>
            <input type="number" step="0.5" min="0" value={f.risePerDay} onInput={set('risePerDay')} />
          </Field>
          <Field label={t('dash.staleMinutes')} hint={t('dash.staleHint')}>
            <input type="number" step="10" min="0" value={f.staleMinutes} onInput={set('staleMinutes')} />
          </Field>
        </div>
      </Card>

      <p class="callout small">{t('dash.privacy')}</p>
      {err && (
        <p class="result result-error" role="alert">
          {err}
        </p>
      )}
      <div class="dash-setup-actions">
        {onForget && (
          <Button variant="danger-ghost" onClick={onForget}>
            {t('dash.forget')}
          </Button>
        )}
        <span class="spacer" />
        {onCancel && <Button onClick={onCancel}>{t('common.cancel')}</Button>}
        <Button type="submit" variant="primary">
          {t('dash.show')}
        </Button>
      </div>
    </form>
  );
}
