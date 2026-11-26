import { useI18n, tOr } from '../components/i18n.js';
import { useStatus, useTick } from '../components/hooks.js';
import { Card, Badge, Meter, Loading, Icon, KV, PageHead } from '../components/ui.jsx';
import { SiloView, TempLegend } from '../components/SiloView.jsx';
import { batteryPercent, batteryLevel, batteryForecastText } from '../lib/battery.js';
import { formatDuration, formatClock, formatEpoch, rssiBars } from '../lib/format.js';
import { pointState, formatTemp, tempColor, tempStats } from '../lib/temp.js';
import { POINT_COUNT } from '../lib/silo.js';
import { powerTone, updateTone } from '../lib/power.js';

const PLACEHOLDER = Array.from({ length: POINT_COUNT }, (_, i) => ({
  point: i + 1,
  rom: null,
  height_cm: null,
  status: 'not_mapped',
  temp_c: null,
}));

function Bars({ n }) {
  return (
    <span class="bars" aria-hidden="true">
      {[1, 2, 3, 4].map((i) => (
        <span key={i} class={i <= n ? 'on' : ''} style={{ height: 4 + i * 3 + 'px' }} />
      ))}
    </span>
  );
}

function BatteryGlyph({ pct, level }) {
  const w = pct === null ? 0 : Math.max(2, (pct / 100) * 26);
  return (
    <svg width="40" height="22" viewBox="0 0 40 22" aria-hidden="true" class={'batt batt-' + level}>
      <rect x="1.5" y="2.5" width="33" height="17" rx="4" fill="none" stroke="currentColor" stroke-width="2" />
      <rect x="35.5" y="8" width="3" height="6" rx="1.2" fill="currentColor" />
      <rect x="5" y="6" width={w} height="10" rx="2" class="batt-fill" />
    </svg>
  );
}

export function PointList({ points }) {
  const { t } = useI18n();
  return (
    <ul class="pointlist">
      {points.map((p) => {
        const st = pointState(p);
        return (
          <li key={p.point} class={'pl-' + st.state}>
            <span class="pl-dot" style={st.valid ? { background: tempColor(st.temp) } : undefined} />
            <span class="pl-num">{p.point}</span>
            <span class="pl-temp">
              {st.valid ? formatTemp(st.temp, 2) : st.state === 'unmapped' ? t('silo.unmapped') : '—'}
            </span>
            <span class="pl-status">
              {st.state === 'error' && <Badge tone="danger">{tOr(t, 'status.', p.status)}</Badge>}
              {typeof p.height_cm === 'number' && p.height_cm >= 0 && <span class="muted">{p.height_cm} cm</span>}
            </span>
          </li>
        );
      })}
    </ul>
  );
}

export function Overview() {
  const { t, lang } = useI18n();
  const { status: s, fetchedAt } = useStatus();
  useTick(1000);
  if (!s) return <Loading text={t('common.loading')} />;

  const since = fetchedAt ? (Date.now() - fetchedAt) / 1000 : 0;
  const m = s.measurement;
  const points = m && m.points && m.points.length ? m.points : PLACEHOLDER;
  const stats = m ? tempStats(m.points) : null;
  const errCount = m ? m.points.filter((p) => pointState(p).state === 'error').length : 0;
  const pct = batteryPercent(s.battery_mv);
  const level = batteryLevel(pct);
  const lu = s.last_upload;
  const b = s.buffer || {};
  const fill = b.capacity ? b.count / b.capacity : 0;
  const sch = s.schedule || {};
  const nextIn = typeof sch.next_measure_in_s === 'number' ? Math.max(0, sch.next_measure_in_s - since) : null;
  const wifi = s.wifi || {};
  const local = s.time_valid ? formatEpoch(s.epoch + Math.floor(since), lang === 'pl' ? 'pl-PL' : 'en-GB') : null;

  return (
    <div>
      <PageHead title={t('nav.overview')} sub={t('overview.sub')} />
      {(!s.config_ok || !s.upload_configured) && (
        <p class="notice notice-warn">
          <Icon name="warn" size={18} />
          <span>{!s.config_ok ? t('overview.configBad') : t('overview.uploadNotConfigured')}</span>
          <a href="#/config">{t('nav.config')} →</a>
        </p>
      )}
      {s.power && s.power.frost_charging && (
        <p class="notice notice-warn" role="alert">
          <Icon name="warn" size={18} />
          <span>
            {t('power.frostCharging', {
              temp: typeof s.power.board_temp_c === 'number' ? s.power.board_temp_c.toFixed(1) : '?',
            })}
          </span>
        </p>
      )}
      {s.alarm && (
        <Card title={t('alarm.title')} icon="warn">
          <Badge tone={s.alarm.active ? 'danger' : 'ok'}>{t(s.alarm.active ? 'alarm.active' : 'alarm.quiet')}</Badge>
          {s.alarm.active && (
            <p>
              {t('alarm.detail', {
                point: s.alarm.point,
                temp: formatTemp(s.alarm.temp_c),
                rise: formatTemp(s.alarm.rise_c),
              })}{' '}
              · {t('alarm.' + s.alarm.reason)}
            </p>
          )}
        </Card>
      )}
      <div class="overview">
        <Card
          class="silo-card"
          title={t('overview.probe')}
          icon="sensors"
          actions={
            m ? (
              <span class="muted small">{t('overview.measuredAgo', { age: formatDuration(m.age_s + since) })}</span>
            ) : null
          }
        >
          {!m && <p class="notice">{t('overview.noMeasurement')}</p>}
          <div class="silo-wrap">
            <SiloView points={points} />
            <div class="silo-side">
              {stats ? (
                <div class="stats3">
                  <div>
                    <small>{t('overview.min')}</small>
                    <b>{formatTemp(stats.min)}</b>
                  </div>
                  <div>
                    <small>{t('overview.mean')}</small>
                    <b>{formatTemp(stats.mean)}</b>
                  </div>
                  <div>
                    <small>{t('overview.max')}</small>
                    <b>{formatTemp(stats.max)}</b>
                  </div>
                </div>
              ) : (
                m && <p class="muted">{t('overview.noValid')}</p>
              )}
              {errCount > 0 && (
                <p class="notice notice-danger">
                  <Icon name="warn" size={16} />
                  <span>{t('overview.pointErrors', { n: errCount })}</span>
                </p>
              )}
              <PointList points={points} />
              {m && m.unmapped && m.unmapped.length > 0 && (
                <p class="muted small">
                  {t('overview.unmappedSensors', { n: m.unmapped.length })} <a href="#/sensors">{t('nav.sensors')} →</a>
                </p>
              )}
            </div>
          </div>
          <TempLegend />
        </Card>

        <div class="tiles">
          <Card title={t('overview.battery')} icon="battery" class="tile">
            <div class="tile-main">
              <BatteryGlyph pct={pct} level={level} />
              <span class="big">{pct === null ? '—' : '~' + pct + ' %'}</span>
            </div>
            <p class="muted small">
              {typeof s.battery_mv === 'number' ? (s.battery_mv / 1000).toFixed(3) + ' V' : t('common.unknown')}
              {level === 'low' || level === 'critical' ? ' · ' + t('overview.battLow') : ''}
            </p>
            <p class="small">{batteryForecastText(s.battery_forecast, t)}</p>
            {s.power && s.power.charging && <p class="small">{t('power.charging')}</p>}
            {s.power && (
              <p class="small">
                <Badge tone={powerTone(s.power.mode)}>{tOr(t, 'power.', s.power.mode)}</Badge>{' '}
                <span class="muted">
                  {t('power.thresholds', { low: s.power.low_mv || '—', crit: s.power.crit_mv || '—' })}
                </span>
              </p>
            )}
          </Card>

          <Card title="Wi-Fi" icon="wifi" class="tile">
            <div class="tile-main">
              <Bars n={wifi.state === 'connected' ? rssiBars(wifi.rssi) : 0} />
              <span class="big">{tOr(t, 'wifi.', wifi.state)}</span>
            </div>
            <p class="muted small">
              {wifi.ssid ? wifi.ssid : t('overview.noSsid')}
              {typeof wifi.rssi === 'number' && wifi.rssi < 0 ? ` · ${wifi.rssi} dBm` : ''}
            </p>
          </Card>

          <Card title={t('overview.lastUpload')} icon="upload" class="tile">
            {lu ? (
              <>
                <div class="tile-main">
                  <Badge tone={lu.ok ? 'ok' : 'danger'}>{lu.ok ? t('common.ok') : t('common.failed')}</Badge>
                  <span class="big">{t('common.ago', { age: formatDuration(lu.age_s + since) })}</span>
                </div>
                <p class="muted small clamp">{lu.message}</p>
              </>
            ) : (
              <p class="muted">{t('upload.never')}</p>
            )}
          </Card>

          <Card title={t('overview.buffer')} icon="db" class="tile">
            <div class="tile-main">
              <span class="big">
                {b.count ?? '—'} <small>/ {b.capacity ?? '—'}</small>
              </span>
            </div>
            <Meter
              value={fill}
              tone={fill > 0.9 ? 'danger' : fill > 0.7 ? 'warn' : 'accent'}
              label={t('overview.buffer')}
            />
            <p class="muted small">
              {t('overview.dropped', { n: (b.dropped_overflow || 0) + (b.dropped_no_time || 0) })}
            </p>
          </Card>

          <Card title={t('overview.nextMeasure')} icon="clock" class="tile">
            <div class="tile-main">
              <span class="big mono">{nextIn === null ? '—' : formatClock(nextIn)}</span>
              {sch.upload_due && <Badge tone="warn">{t('overview.uploadDue')}</Badge>}
            </div>
            <p class="muted small">
              {t('overview.every', { m: formatDuration(sch.measure_s), u: formatDuration(sch.upload_s) })}
            </p>
          </Card>

          <Card title={t('overview.device')} icon="chip" class="tile">
            <KV
              items={[
                [t('common.firmware'), s.firmware],
                [t('overview.time'), s.time_valid ? local : <Badge tone="warn">{t('overview.timeInvalid')}</Badge>],
                [t('overview.reset'), s.reset_reason],
                s.remote_update && [
                  t('remote.title'),
                  s.remote_update.enabled ? (
                    <Badge key="ru" tone={updateTone(s.remote_update.result)}>
                      {tOr(t, 'remote.', s.remote_update.result)}
                      {s.remote_update.latest ? ' · ' + s.remote_update.latest : ''}
                    </Badge>
                  ) : (
                    t('remote.disabled')
                  ),
                ],
              ]}
            />
          </Card>
        </div>
      </div>
    </div>
  );
}
