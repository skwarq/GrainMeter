import { useMemo, useState } from 'preact/hooks';
import { tOr, useI18n } from '../components/i18n.js';
import { pointLabel } from '../components/History.jsx';
import { formatDuration } from '../lib/format.js';
import { BATTERY_STEP_PERCENT, collectChanges, DATA_GAP_MS, TEMPERATURE_CHANGE_C, visibleChanges } from './changes.js';

export function ChangesCard({ main, diag }) {
  const { t, lang } = useI18n();
  const [type, setType] = useState('all');
  const changes = useMemo(() => collectChanges(main, diag), [main, diag]);
  const { rows, total } = visibleChanges(changes, type);
  const time = (ms) => new Date(ms).toLocaleString(lang === 'en' ? 'en-GB' : 'pl-PL');
  const value = (c, v) => {
    if (v === null) return t('changes.none');
    if (c.kind === 'temperature') return v.toFixed(1).replace('.', lang === 'en' ? '.' : ',') + ' °C';
    if (c.kind === 'battery') return '~' + v + ' %';
    if (c.kind === 'power') return tOr(t, 'power.', v);
    if (c.kind === 'sensor') return tOr(t, 'status.', v);
    if (c.kind === 'alarm') return tOr(t, 'changes.alarm.', v);
    if (c.kind === 'gap') return time(v);
    return v;
  };
  return (
    <details class="card dash-changes">
      <summary>{t('changes.title')}</summary>
      <p class="muted small">
        {t('changes.thresholds', {
          temperature: String(TEMPERATURE_CHANGE_C).replace('.', lang === 'en' ? '.' : ','),
          battery: BATTERY_STEP_PERCENT,
          gap: formatDuration(DATA_GAP_MS / 1000),
        })}
      </p>
      <p class="muted small">{t('changes.evidence')}</p>
      <label class="changes-filter">
        {t('changes.filter')}
        <select value={type} onChange={(e) => setType(e.currentTarget.value)}>
          {['all', 'temperature', 'battery', 'device'].map((key) => (
            <option key={key} value={key}>
              {t('changes.type.' + key)}
            </option>
          ))}
        </select>
      </label>
      <p class="muted small" role="status">
        {t('changes.count', { n: rows.length, total })}
      </p>
      {rows.length ? (
        <div class="table-wrap" role="region" aria-label={t('changes.title')} tabIndex={0}>
          <table class="dash-table">
            <thead>
              <tr>
                <th>{t('changes.time')}</th>
                <th>{t('dash.colPoint')}</th>
                <th>{t('changes.kind')}</th>
                <th>{t('changes.values')}</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((c, i) => (
                <tr key={i}>
                  <td class="changes-time">{time(c.t)}</td>
                  <td>{c.point ? pointLabel(t, main, c.point - 1) : t('changes.type.' + c.type)}</td>
                  <td>
                    {t('changes.kind.' + c.kind)}
                    {c.kind === 'gap' && ' · ' + formatDuration((c.after - c.before) / 1000)}
                  </td>
                  <td class={c.kind === 'temperature' ? (c.after > c.before ? 'changes-rise' : 'changes-fall') : ''}>
                    {value(c, c.before)} → {value(c, c.after)}
                    {c.kind === 'temperature' && (c.after > c.before ? ' ↑' : ' ↓')}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : (
        <p class="muted">{t('changes.empty')}</p>
      )}
    </details>
  );
}
