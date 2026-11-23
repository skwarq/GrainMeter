import { useEffect, useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n, errorText } from '../components/i18n.js';
import { useAction, useLoad, useStatus } from '../components/hooks.js';
import { Card, Button, Result, Loading, PageHead, Badge } from '../components/ui.jsx';
import { describeError } from '../lib/errors.js';
import { formatDuration, samplesPerUpload } from '../lib/format.js';
import { validateNetworkAndPower } from '../lib/power.js';

const TEXT_FIELDS = [
  'name',
  'wifi_ssid',
  'report_url',
  'ts_channel',
  'diag_channel',
  'measure_s',
  'upload_s',
  'diag_every_h',
  'batt_low_mv',
  'batt_crit_mv',
  'update_url',
  'update_check_h',
  'static_ip',
  'gateway',
  'netmask',
  'dns',
];
const SECRET_FIELDS = ['wifi_pass', 'ts_key', 'diag_key'];

function fromConfig(c) {
  const f = {};
  for (const k of TEXT_FIELDS) f[k] = c[k] === undefined || c[k] === null ? '' : String(c[k]);
  for (const k of SECRET_FIELDS) f[k] = '';
  f.wifi_pass_clear = false;
  f.svc_pass = '';
  f.svc_pass2 = '';
  return f;
}

function Field({ label, hint, children, wide }) {
  return (
    <label class={'field' + (wide ? ' field-wide' : '')}>
      <span>{label}</span>
      {children}
      {hint && <small class="hint">{hint}</small>}
    </label>
  );
}

export function Config() {
  const { t } = useI18n();
  const { refresh } = useStatus();
  const cfg = useLoad(() => api.getConfig());
  const act = useAction();
  const [f, setF] = useState(null);
  const [localErr, setLocalErr] = useState('');

  useEffect(() => {
    if (cfg.data) setF(fromConfig(cfg.data));
  }, [cfg.data]);

  if (cfg.error && !cfg.data) return <p class="result result-error">{errorText(t, describeError(cfg.error))}</p>;
  if (!f || !cfg.data) return <Loading text={t('common.loading')} />;
  const c = cfg.data;

  const set = (k) => (e) => {
    const el = e.currentTarget;
    setF({ ...f, [k]: el.type === 'checkbox' ? el.checked : el.value });
  };
  const secret = (k, isSet) => ({
    type: 'password',
    autocomplete: 'new-password',
    value: f[k],
    onInput: set(k),
    placeholder: isSet ? t('config.secretSet') : t('config.secretUnset'),
  });

  const spu = samplesPerUpload(f.measure_s, f.upload_s);

  const submit = (e) => {
    e.preventDefault();
    setLocalErr('');
    if (f.svc_pass && f.svc_pass !== f.svc_pass2) {
      setLocalErr(t('config.svcMismatch'));
      act.setResult(null);
      return;
    }
    const netErr = validateNetworkAndPower(f);
    if (netErr) {
      setLocalErr(t(netErr));
      act.setResult(null);
      return;
    }
    const form = {};
    for (const k of TEXT_FIELDS) form[k] = f[k].trim();
    for (const k of SECRET_FIELDS) if (f[k]) form[k] = f[k];
    if (f.wifi_pass_clear) {
      form.wifi_pass_clear = 1;
      delete form.wifi_pass;
    }
    if (f.svc_pass) form.svc_pass = f.svc_pass;
    act
      .run(() => api.saveConfig(form), { ok: (r) => r.message || t('config.saved') })
      .then((r) => {
        if (r) {
          cfg.reload();
          refresh();
        }
      });
  };

  return (
    <form onSubmit={submit} autocomplete="off">
      <PageHead title={t('nav.config')} sub={t('config.sub')} />

      <Card title={t('config.device')} icon="chip">
        <div class="form-grid">
          <Field label={t('config.name')}>
            <input type="text" maxLength={31} value={f.name} onInput={set('name')} />
          </Field>
        </div>
      </Card>

      <Card title="Wi-Fi" icon="wifi">
        <div class="form-grid">
          <Field label={t('config.ssid')}>
            <input
              type="text"
              maxLength={32}
              value={f.wifi_ssid}
              onInput={set('wifi_ssid')}
              autocapitalize="off"
              spellcheck={false}
            />
          </Field>
          <Field label={t('config.wifiPass')} hint={t('config.secretHint')}>
            <input {...secret('wifi_pass', c.wifi_pass_set)} disabled={f.wifi_pass_clear} />
          </Field>
          <label class="check field-wide">
            <input type="checkbox" checked={f.wifi_pass_clear} onChange={set('wifi_pass_clear')} />
            <span>{t('config.wifiOpen')}</span>
          </label>
          <p class="muted small field-wide">{t('config.staticHint')}</p>
          <Field label={t('config.staticIp')}>
            <input
              type="text"
              inputMode="decimal"
              maxLength={15}
              placeholder="a.b.c.d"
              value={f.static_ip}
              onInput={set('static_ip')}
              autocapitalize="off"
              spellcheck={false}
            />
          </Field>
          <Field label={t('config.gateway')}>
            <input
              type="text"
              inputMode="decimal"
              maxLength={15}
              placeholder="a.b.c.d"
              value={f.gateway}
              onInput={set('gateway')}
              autocapitalize="off"
              spellcheck={false}
            />
          </Field>
          <Field label={t('config.netmask')}>
            <input
              type="text"
              inputMode="decimal"
              maxLength={15}
              placeholder="a.b.c.d"
              value={f.netmask}
              onInput={set('netmask')}
              autocapitalize="off"
              spellcheck={false}
            />
          </Field>
          <Field label={t('config.dns')}>
            <input
              type="text"
              inputMode="decimal"
              maxLength={15}
              placeholder="a.b.c.d"
              value={f.dns}
              onInput={set('dns')}
              autocapitalize="off"
              spellcheck={false}
            />
          </Field>
        </div>
      </Card>

      <Card title="ThingSpeak" icon="upload">
        <div class="form-grid">
          <Field label={t('config.reportUrl')} hint={t('config.reportUrlHint')} wide>
            <input
              type="url"
              value={f.report_url}
              onInput={set('report_url')}
              autocapitalize="off"
              spellcheck={false}
            />
          </Field>
          <Field label={t('config.channel')}>
            <input type="number" inputMode="numeric" min="0" value={f.ts_channel} onInput={set('ts_channel')} />
          </Field>
          <Field label={t('config.writeKey')} hint={t('config.keyHint')}>
            <input {...secret('ts_key', c.ts_key_set)} autocapitalize="characters" />
          </Field>
          <Field label={t('config.diagChannel')} hint={t('config.diagHint')}>
            <input type="number" inputMode="numeric" min="0" value={f.diag_channel} onInput={set('diag_channel')} />
          </Field>
          <Field label={t('config.diagKey')} hint={t('config.keyHint')}>
            <input {...secret('diag_key', c.diag_key_set)} autocapitalize="characters" />
          </Field>
          <Field label={t('config.diagEvery')} hint={t('config.diagEveryHint')}>
            <input
              type="number"
              inputMode="numeric"
              min="0"
              max="168"
              value={f.diag_every_h}
              onInput={set('diag_every_h')}
            />
          </Field>
        </div>
      </Card>

      <Card title={t('config.power')} icon="battery">
        <div class="form-grid">
          <Field label={t('config.battLow')} hint={t('config.battLowHint')}>
            <input
              type="number"
              inputMode="numeric"
              min="0"
              max="4500"
              value={f.batt_low_mv}
              onInput={set('batt_low_mv')}
            />
          </Field>
          <Field label={t('config.battCrit')} hint={t('config.battCritHint')}>
            <input
              type="number"
              inputMode="numeric"
              min="0"
              max="4500"
              value={f.batt_crit_mv}
              onInput={set('batt_crit_mv')}
            />
          </Field>
        </div>
      </Card>

      <Card title={t('config.remote')} icon="update">
        <div class="form-grid">
          <Field label={t('config.updateUrl')} hint={t('config.updateUrlHint')} wide>
            <input
              type="url"
              value={f.update_url}
              onInput={set('update_url')}
              placeholder="https://…/manifest.json"
              autocapitalize="off"
              spellcheck={false}
            />
          </Field>
          <Field label={t('config.updateCheck')} hint={t('config.updateCheckHint')}>
            <input
              type="number"
              inputMode="numeric"
              min="0"
              max="720"
              value={f.update_check_h}
              onInput={set('update_check_h')}
            />
          </Field>
        </div>
      </Card>

      <Card title={t('config.schedule')} icon="clock">
        <div class="form-grid">
          <Field label={t('config.measureS')} hint={formatDuration(Number(f.measure_s))}>
            <input type="number" inputMode="numeric" min="1" value={f.measure_s} onInput={set('measure_s')} />
          </Field>
          <Field label={t('config.uploadS')} hint={formatDuration(Number(f.upload_s))}>
            <input type="number" inputMode="numeric" min="15" value={f.upload_s} onInput={set('upload_s')} />
          </Field>
        </div>
        <p class="callout">
          {spu === null ? (
            t('config.spuInvalid')
          ) : (
            <>
              {t('config.spu')} <b>{spu}</b>
              {Number(f.upload_s) < Number(f.measure_s) && (
                <>
                  {' '}
                  <Badge tone="warn">{t('config.uploadLtMeasure')}</Badge>
                </>
              )}
            </>
          )}
        </p>
      </Card>

      <Card title={t('config.service')} icon="lock">
        <div class="form-grid">
          <Field label={t('config.svcPass')} hint={t('config.svcHint')}>
            <input
              type="password"
              autocomplete="new-password"
              value={f.svc_pass}
              onInput={set('svc_pass')}
              placeholder={c.svc_pass_set ? t('config.secretSet') : t('config.secretUnset')}
            />
          </Field>
          <Field label={t('config.svcPass2')}>
            <input type="password" autocomplete="new-password" value={f.svc_pass2} onInput={set('svc_pass2')} />
          </Field>
        </div>
      </Card>

      <div class="savebar">
        {localErr && (
          <p class="result result-error" role="alert">
            {localErr}
          </p>
        )}
        <Result result={act.result} />
        <div class="savebar-actions">
          <Button onClick={() => setF(fromConfig(c))} disabled={act.busy}>
            {t('config.reset')}
          </Button>
          <Button type="submit" variant="primary" busy={act.busy}>
            {t('common.save')}
          </Button>
        </div>
      </div>
    </form>
  );
}
