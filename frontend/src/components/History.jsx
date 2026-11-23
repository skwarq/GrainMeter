// History views shared by the device portal (local history from flash) and
// the remote dashboard (ThingSpeak). Both pass a "feed" from lib/feed.js.
import { useState } from 'preact/hooks';
import { useI18n, tOr } from './i18n.js';
import { Card } from './ui.jsx';
import { LineChart } from './LineChart.jsx';
import {
  FIELD_COUNT,
  RANGES,
  alerts as findAlerts,
  change24h,
  gapThreshold,
  hasData,
  latestPoints,
  pointStats,
  segments,
} from '../lib/feed.js';
import { formatTemp } from '../lib/temp.js';
import { formatDuration } from '../lib/format.js';

export const COLORS = Array.from({ length: FIELD_COUNT }, (_, i) => `var(--p${i + 1})`);

/** Default alert thresholds (the dashboard lets the user change them). */
export const ALERT_DEFAULTS = { maxTemp: 25, risePerDay: 3, staleMinutes: 150 };

export function pointLabel(t, feed, i) {
  const label = feed && feed.fields[i].label;
  return label || t('dash.point', { n: i + 1 });
}

export function signed(v, digits = 1) {
  if (v === null || !Number.isFinite(v)) return '—';
  return (v > 0 ? '+' : '') + v.toFixed(digits);
}

/** Range selector 24 h / 7 d / 30 d. */
export function RangeTabs({ range, onRange }) {
  const { t } = useI18n();
  return (
    <div class="seg" role="tablist">
      {Object.keys(RANGES).map((r) => (
        <button
          key={r}
          type="button"
          role="tab"
          aria-selected={r === range}
          class={r === range ? 'on' : ''}
          onClick={() => onRange(r)}
        >
          {t('dash.range.' + r)}
        </button>
      ))}
    </div>
  );
}

/** Multi-point temperature chart with per-point toggles. */
export function HistoryCard({ feed, range, onRange, now, title, class: cls }) {
  const { t, lang } = useI18n();
  const [hidden, setHidden] = useState(() => new Set());
  const toggle = (i) => {
    const n = new Set(hidden);
    if (n.has(i)) n.delete(i);
    else n.add(i);
    setHidden(n);
  };
  return (
    <Card
      title={title || t('dash.history')}
      icon="diag"
      class={'dash-history' + (cls ? ' ' + cls : '')}
      actions={<RangeTabs range={range} onRange={onRange} />}
    >
      <div class="chips">
        {feed.fields.map((f, i) =>
          hasData(feed, i) ? (
            <button
              key={i}
              type="button"
              class={'chip' + (hidden.has(i) ? ' off' : '')}
              aria-pressed={!hidden.has(i)}
              onClick={() => toggle(i)}
            >
              <span class="dot" style={{ background: COLORS[i] }} />
              {pointLabel(t, feed, i)}
            </button>
          ) : null,
        )}
      </div>
      <LineChart
        lang={lang}
        start={now - RANGES[range]}
        end={now}
        emptyText={t('dash.noData', { range: t('dash.range.' + range) })}
        series={feed.fields
          .map((f, i) => ({
            key: 'p' + i,
            label: pointLabel(t, feed, i),
            color: COLORS[i],
            segments: hasData(feed, i) && !hidden.has(i) ? segments(feed, i, gapThreshold(feed)) : [],
          }))
          .filter((s) => s.segments.length)}
      />
    </Card>
  );
}

/** Current / 24 h change / min / max per point. */
export function PointTable({ feed, settings, points }) {
  const { t } = useI18n();
  const latest = points || latestPoints(feed);
  return (
    <table class="dash-table">
      <thead>
        <tr>
          <th>{t('dash.colPoint')}</th>
          <th>{t('dash.colNow')}</th>
          <th>{t('dash.col24h')}</th>
          <th>{t('dash.colMin')}</th>
          <th>{t('dash.colMax')}</th>
        </tr>
      </thead>
      <tbody>
        {feed.fields.map((f, i) => {
          const p = latest[i] || { temp_c: null, status: 'not_mapped' };
          if (!hasData(feed, i) && p.status === 'not_mapped') return null;
          const s = pointStats(feed, i);
          const d = change24h(feed, i);
          const rising = settings.risePerDay && d !== null && d >= settings.risePerDay;
          const hot =
            settings.maxTemp !== null &&
            settings.maxTemp !== undefined &&
            p.temp_c !== null &&
            p.temp_c >= settings.maxTemp;
          return (
            <tr key={i}>
              <td class="pt" title={f.label || t('dash.point', { n: i + 1 })}>
                <span class="dot" style={{ background: COLORS[i] }} />
                <b>{i + 1}</b>
                {f.heightCm !== null && <span class="muted small"> {f.heightCm} cm</span>}
              </td>
              <td class={'num' + (hot ? ' text-danger' : '')}>
                {p.temp_c !== null && p.temp_c !== undefined ? (
                  formatTemp(p.temp_c)
                ) : (
                  <span class="text-danger">{tOr(t, 'status.', p.status)}</span>
                )}
              </td>
              <td class={'num' + (rising ? ' text-danger' : '')}>{signed(d)}</td>
              <td class="num">{formatTemp(s.min)}</td>
              <td class="num">{formatTemp(s.max)}</td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

/** Alert list (alarm temperature, self-heating, sensor errors, stale data). */
export function AlertsCard({ feed, settings, now }) {
  const { t } = useI18n();
  const list = findAlerts(feed, settings, now);
  if (!list.length) return null;
  return (
    <Card title={t('dash.alertsNow', { n: list.length })} icon="warn" tone="danger">
      <ul class="dash-alerts">
        {list.map((a, k) => (
          <li key={k}>
            {t('dash.alert.' + a.kind, {
              point: a.point ? pointLabel(t, feed, a.point - 1) : '',
              value:
                a.kind === 'stale'
                  ? formatDuration(a.value * 60)
                  : a.kind === 'sensor'
                    ? tOr(t, 'status.', a.value)
                    : a.kind === 'rising'
                      ? signed(a.value)
                      : formatTemp(a.value),
            })}
          </li>
        ))}
      </ul>
    </Card>
  );
}
