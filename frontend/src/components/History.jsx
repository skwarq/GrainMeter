// History views shared by the device portal (local history from flash) and
// the remote dashboard (ThingSpeak). Both pass a "feed" from lib/feed.js.
import { useMemo, useState } from 'preact/hooks';
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
  devicePercentFeed,
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
export function HistoryCard({
  feed,
  range,
  onRange,
  now,
  title,
  class: cls,
  skipMissing = false,
  diag,
  hasDiag = false,
  view = 'temperature',
  onView,
}) {
  const { t, lang } = useI18n();
  const [hidden, setHidden] = useState(() => new Set());
  const [batteryUnits, setBatteryUnits] = useState('pct');
  const [wifiUnits, setWifiUnits] = useState('pct');
  const activeView = onView && hasDiag && ['battery', 'wifi'].includes(view) ? view : 'temperature';
  const temperature = activeView === 'temperature';
  const pct = (activeView === 'battery' ? batteryUnits : wifiUnits) === 'pct';
  const device = useMemo(() => {
    const volts = {
      entries: (diag?.entries || []).map((e) => ({
        ...e,
        values: [e.values[0] === null ? null : e.values[0] / 1000, e.values[1]],
      })),
    };
    return pct ? devicePercentFeed(volts) : volts;
  }, [diag, pct]);
  const deviceLabel = t(activeView === 'battery' ? 'overview.battery' : pct ? 'hist.signalQuality' : 'hist.signal');
  const toggle = (i) => {
    const n = new Set(hidden);
    if (n.has(i)) n.delete(i);
    else n.add(i);
    setHidden(n);
  };
  return (
    <Card
      title={title || t(temperature ? 'dash.history' : 'dash.history.' + activeView)}
      icon="diag"
      class={'dash-history' + (cls ? ' ' + cls : '')}
      actions={
        <div class="history-controls">
          {onView && (
            <div class="seg history-views" role="tablist" aria-label={t('dash.historyView')}>
              {['temperature', 'battery', 'wifi'].map((v) => {
                const disabled = v !== 'temperature' && !hasDiag;
                return (
                  <button
                    key={v}
                    type="button"
                    role="tab"
                    disabled={disabled}
                    title={disabled ? t('dash.historyNeedsDiag') : undefined}
                    aria-describedby={disabled ? 'history-diag-hint' : undefined}
                    aria-selected={v === activeView}
                    class={v === activeView ? 'on' : ''}
                    onClick={() => !disabled && onView(v)}
                  >
                    {t('dash.historyView.' + v)}
                  </button>
                );
              })}
            </div>
          )}
          <RangeTabs range={range} onRange={onRange} />
        </div>
      }
    >
      {onView && !hasDiag && (
        <p id="history-diag-hint" class="muted small">
          {t('dash.historyNeedsDiag')}
        </p>
      )}
      {temperature ? (
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
      ) : (
        <div class="history-units">
          <div class="seg" role="tablist" aria-label={deviceLabel + ': ' + t('hist.units')}>
            {[
              ['pct', '%'],
              ['raw', activeView === 'battery' ? 'V' : 'dBm'],
            ].map(([u, label]) => (
              <button
                key={u}
                type="button"
                role="tab"
                aria-selected={pct === (u === 'pct')}
                class={pct === (u === 'pct') ? 'on' : ''}
                onClick={() => (activeView === 'battery' ? setBatteryUnits : setWifiUnits)(u)}
              >
                {label}
              </button>
            ))}
          </div>
          {pct && <p class="muted small">{t('dash.historyHint.' + activeView)}</p>}
        </div>
      )}
      <LineChart
        key={activeView + (temperature ? '' : pct ? '-pct' : '-raw')}
        lang={lang}
        unit={temperature ? '°C' : pct ? '%' : activeView === 'battery' ? 'V' : 'dBm'}
        digits={temperature ? 1 : pct || activeView === 'wifi' ? 0 : 2}
        domain={!temperature && pct ? [0, 100] : undefined}
        start={now - RANGES[range]}
        end={now}
        emptyText={t('dash.noData', { range: t('dash.range.' + range) })}
        series={
          temperature
            ? feed.fields
                .map((f, i) => ({
                  key: 'p' + i,
                  label: pointLabel(t, feed, i),
                  color: COLORS[i],
                  segments:
                    hasData(feed, i) && !hidden.has(i) ? segments(feed, i, gapThreshold(feed), skipMissing) : [],
                }))
                .filter((s) => s.segments.length)
            : [
                {
                  key: activeView,
                  label: deviceLabel,
                  color: activeView === 'battery' ? COLORS[0] : COLORS[4],
                  segments: segments(device, activeView === 'battery' ? 0 : 1, diag ? gapThreshold(diag) * 8 : 0, true),
                },
              ]
        }
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

const UNITS_KEY = 'gm.deviceUnits';

function initialUnits() {
  try {
    const u = localStorage.getItem(UNITS_KEY);
    if (u === 'pct' || u === 'raw') return u;
  } catch {
    /* ignore */
  }
  return 'pct';
}

/**
 * Battery and Wi-Fi signal over time. `feed` entries carry values
 * [battery V, RSSI dBm] (lib/feed.js deviceFeed() or the diag channel).
 * The user switches between percent (easier to read) and V / dBm.
 */
export function DeviceCharts({ feed, start, end, gapMs, skipMissing = false }) {
  const { t, lang } = useI18n();
  const [units, setUnitsState] = useState(initialUnits);
  const setUnits = (u) => {
    setUnitsState(u);
    try {
      localStorage.setItem(UNITS_KEY, u);
    } catch {
      /* ignore */
    }
  };
  const pct = units === 'pct';
  // The device history already knows its own gap limit; the ThingSpeak diag
  // channel reports only every few hours.
  const gap = gapMs || feed.maxGapMs || gapThreshold(feed) * 8;
  const shown = useMemo(() => (pct ? devicePercentFeed(feed) : feed), [feed, pct]);
  const chart = (i, key, label, unit, digits, color) => (
    <div class="device-chart">
      <h3 class="device-chart-title">{label}</h3>
      <LineChart
        lang={lang}
        height={150}
        unit={unit}
        digits={digits}
        start={start}
        end={end}
        domain={pct ? [0, 100] : undefined}
        emptyText={t('hist.noDeviceData')}
        series={[{ key, label, color, segments: segments(shown, i, gap, skipMissing) }]}
      />
    </div>
  );
  return (
    <div class="device-charts-wrap">
      <div class="device-charts-head">
        <div class="seg" role="tablist" aria-label={t('hist.units')}>
          {[
            ['pct', '%'],
            ['raw', 'V · dBm'],
          ].map(([u, label]) => (
            <button
              key={u}
              type="button"
              role="tab"
              aria-selected={units === u}
              class={units === u ? 'on' : ''}
              onClick={() => setUnits(u)}
            >
              {label}
            </button>
          ))}
        </div>
        {pct && <p class="muted small">{t('hist.pctHint')}</p>}
      </div>
      <div class="device-charts">
        {chart(0, 'bat', t('overview.battery'), pct ? '%' : 'V', pct ? 0 : 2, 'var(--p1)')}
        {chart(1, 'rssi', pct ? t('hist.signalQuality') : t('hist.signal'), pct ? '%' : 'dBm', 0, 'var(--p5)')}
      </div>
    </div>
  );
}
