import { useCallback, useEffect, useMemo, useState } from 'preact/hooks';
import { translate, DEFAULT_LANG, LANGS } from '../i18n/translate.js';
import { I18nContext, tOr, useI18n } from '../components/i18n.js';
import { Badge, Button, Card, KV, Loading, Logo } from '../components/ui.jsx';
import { ALERT_DEFAULTS, AlertsCard, DeviceCharts, HistoryCard, PointTable } from '../components/History.jsx';
import { SiloView, TempLegend } from '../components/SiloView.jsx';
import { batteryPercent, batteryForecast, batteryForecastText } from '../lib/battery.js';
import { formatDuration } from '../lib/format.js';
import { RANGES, alerts as findAlerts, gapThreshold, latestDiag, latestPoints } from '../lib/feed.js';
import { powerTone } from '../lib/power.js';
import { Setup } from './Setup.jsx';
import { clearSettings, loadSettings, saveSettings } from './settings.js';
import { loadFeed } from './thingspeak.js';

const REFRESH_MS = 5 * 60 * 1000;
const LANG_KEY = 'gm.lang';

function initialLang() {
  try {
    const l = localStorage.getItem(LANG_KEY);
    if (LANGS.includes(l)) return l;
  } catch {
    /* ignore */
  }
  return navigator.language && navigator.language.startsWith('pl') ? 'pl' : DEFAULT_LANG;
}

export function App() {
  const [lang, setLangState] = useState(initialLang);
  const [settings, setSettings] = useState(loadSettings);
  const [editing, setEditing] = useState(false);
  const [state, setState] = useState({ loading: false, main: null, diag: null, error: null, diagError: null });
  const [now, setNow] = useState(Date.now());

  const i18n = useMemo(
    () => ({
      lang,
      t: (k, p) => translate(lang, k, p),
      setLang: (l) => {
        setLangState(l);
        try {
          localStorage.setItem(LANG_KEY, l);
        } catch {
          /* ignore */
        }
      },
    }),
    [lang],
  );
  const { t } = i18n;

  useEffect(() => {
    document.documentElement.lang = lang;
  }, [lang]);

  const range = (settings && settings.range) || '7d';

  const load = useCallback(async () => {
    if (!settings) return;
    setState((s) => ({ ...s, loading: true }));
    const startMs = Date.now() - RANGES[range];
    const main = loadFeed({ channelId: settings.channelId, readKey: settings.readKey, startMs });
    const diag = settings.diagChannelId
      ? loadFeed({ channelId: settings.diagChannelId, readKey: settings.diagReadKey, startMs })
      : Promise.resolve(null);
    const [m, d] = await Promise.allSettled([main, diag]);
    // Nothing in the selected range: fetch the newest entry anyway, so the
    // page still shows when the device last reported and what it measured.
    const newest = (r, channelId, readKey) =>
      r.status === 'fulfilled' && r.value && r.value.entries.length === 0
        ? loadFeed({ channelId, readKey, results: 1 }).catch(() => null)
        : Promise.resolve(null);
    const [mLast, dLast] = await Promise.all([
      newest(m, settings.channelId, settings.readKey),
      settings.diagChannelId ? newest(d, settings.diagChannelId, settings.diagReadKey) : Promise.resolve(null),
    ]);
    setNow(Date.now());
    setState({
      loading: false,
      main: m.status === 'fulfilled' ? m.value : null,
      mainLast: mLast,
      error: m.status === 'rejected' ? m.reason : null,
      diag: d.status === 'fulfilled' ? d.value : null,
      diagLast: dLast,
      diagError: d.status === 'rejected' ? d.reason : null,
    });
  }, [settings, range]);

  // load on start / settings change, refresh every 5 min while visible
  useEffect(() => {
    load();
    const id = setInterval(() => {
      if (document.visibilityState === 'visible') load();
    }, REFRESH_MS);
    const onVis = () => document.visibilityState === 'visible' && load();
    document.addEventListener('visibilitychange', onVis);
    const tick = setInterval(() => setNow(Date.now()), 30000);
    return () => {
      clearInterval(id);
      clearInterval(tick);
      document.removeEventListener('visibilitychange', onVis);
    };
  }, [load]);

  const store = (s) => {
    saveSettings(s);
    setSettings(s);
    setEditing(false);
  };

  if (!settings || editing) {
    return (
      <I18nContext.Provider value={i18n}>
        <div class="dash">
          <Setup
            initial={settings}
            onSave={store}
            onCancel={settings ? () => setEditing(false) : null}
            onForget={
              settings
                ? () => {
                    clearSettings();
                    setSettings(null);
                    setEditing(false);
                    setState({ loading: false, main: null, diag: null, error: null, diagError: null });
                  }
                : null
            }
          />
          <LangSwitch />
        </div>
      </I18nContext.Provider>
    );
  }

  const feed = state.main;
  // Newest data: from the range, or the last entry outside it.
  const inRange = !!(feed && feed.entries.length > 0);
  const current = inRange ? feed : state.mainLast && state.mainLast.entries.length > 0 ? state.mainLast : feed;
  const lastEntry = current && current.entries[current.entries.length - 1];
  const ageS = lastEntry ? Math.max(0, Math.round((now - lastEntry.t) / 1000)) : null;
  const future = lastEntry ? isFuture(lastEntry.t, now) : false;
  const alerts = current ? findAlerts(current, settings, now) : [];
  const stale = alerts.some((a) => a.kind === 'stale');

  return (
    <I18nContext.Provider value={i18n}>
      <div class="dash">
        <header class="dash-head">
          <Logo size={34} />
          <div class="dash-title">
            <h1>{(feed && feed.name) || t('dash.title')}</h1>
            <div class="muted small">
              {lastEntry ? (
                <>
                  {t('dash.lastMeasurement')}{' '}
                  <Badge tone={stale || future ? 'warn' : 'ok'} title={future ? t('dash.futureHint') : undefined}>
                    {future ? t('dash.future') : t('common.ago', { age: formatDuration(ageS) })}
                  </Badge>{' '}
                  <span class="dash-time">
                    {new Date(lastEntry.t).toLocaleString(lang === 'en' ? 'en-GB' : 'pl-PL')}
                  </span>
                </>
              ) : (
                t('dash.channel', { id: settings.channelId })
              )}
            </div>
          </div>
          <div class="dash-actions">
            <Button small icon="refresh" busy={state.loading} onClick={load}>
              {t('common.refresh')}
            </Button>
            <Button small icon="config" onClick={() => setEditing(true)}>
              {t('dash.settings')}
            </Button>
            <LangSwitch />
          </div>
        </header>

        {state.error && (
          <p class="result result-error" role="alert">
            {t('dash.err.' + state.error.code, { status: state.error.status })}
          </p>
        )}
        {!feed && !state.error && <Loading text={t('common.loading')} />}

        {feed && (
          <>
            <AlertsCard feed={current} settings={{ ...ALERT_DEFAULTS, ...settings }} now={now} />

            <div class="dash-grid">
              <Card title={t('dash.siloNow')} icon="overview" class="dash-silo">
                {lastEntry ? (
                  <>
                    {!inRange && (
                      <p class="notice small">
                        {t('dash.outOfRange', {
                          range: t('dash.range.' + range),
                          time: new Date(lastEntry.t).toLocaleString(lang === 'en' ? 'en-GB' : 'pl-PL'),
                        })}
                      </p>
                    )}
                    <div class="silo-wrap">
                      <div>
                        <SiloView points={latestPoints(current)} />
                        <TempLegend />
                      </div>
                      <PointTable feed={current} settings={settings} />
                    </div>
                  </>
                ) : (
                  <p class="muted">{t('dash.channelEmpty')}</p>
                )}
              </Card>

              <HistoryCard feed={feed} range={range} now={now} onRange={(r) => store({ ...settings, range: r })} />

              {settings.diagChannelId && (
                <DeviceCard
                  diag={state.diag}
                  diagLast={state.diagLast}
                  error={state.diagError}
                  t={t}
                  now={now}
                  range={range}
                />
              )}
            </div>
          </>
        )}

        <footer class="dash-foot muted small">
          {t('dash.footer', { id: settings.channelId })} · {t('dash.privacyShort')}
        </footer>
      </div>
    </I18nContext.Provider>
  );
}

function LangSwitch() {
  const { lang, setLang } = useI18n();
  return (
    <button type="button" class="btn btn-sm btn-secondary lang" onClick={() => setLang(lang === 'pl' ? 'en' : 'pl')}>
      <span>{lang === 'pl' ? 'EN' : 'PL'}</span>
    </button>
  );
}

// Timestamps come from the device clock; the PC simulator's clock runs
// SPEED times faster, so its reports can lie ahead of the real time.
const FUTURE_TOLERANCE_MS = 5 * 60 * 1000;
function isFuture(tMs, nowMs) {
  return tMs - nowMs > FUTURE_TOLERANCE_MS;
}

function DeviceCard({ diag, diagLast, error, t, now, range }) {
  if (error) {
    return (
      <Card title={t('dash.device')} icon="chip">
        <p class="result result-error">{t('dash.err.' + error.code, { status: error.status })}</p>
      </Card>
    );
  }
  const d = latestDiag(diag) || latestDiag(diagLast);
  if (!d) {
    return (
      <Card title={t('dash.device')} icon="chip">
        <p class="muted">{t('dash.channelEmpty')}</p>
      </Card>
    );
  }
  const pct = batteryPercent(d.batteryMv);
  // Diagnostic feeds do not publish the configured critical threshold; use the default 3500 mV.
  const forecast = batteryForecast(
    (diag?.entries || []).map((e) => ({ t: e.t, batteryMv: e.values[0] })),
    now,
    3500,
  );
  const forecastText = RANGES[range] < 86400000 ? t('batteryForecast.range') : batteryForecastText(forecast, t);
  return (
    <Card title={t('dash.device')} icon="chip" class="dash-device">
      <KV
        items={[
          [
            t('overview.battery'),
            d.batteryMv === null
              ? '—'
              : `${(d.batteryMv / 1000).toFixed(2)} V${pct === null ? '' : ' · ~' + pct + ' %'}`,
          ],
          [t('batteryForecast.title'), forecastText],
          d.power && [
            t('power.mode'),
            <Badge key="pm" tone={powerTone(d.power)}>
              {tOr(t, 'power.', d.power)}
            </Badge>,
          ],
          ['Wi-Fi', d.rssi === null ? '—' : d.rssi + ' dBm'],
          [
            t('dash.sensors'),
            d.sensors === null
              ? '—'
              : `${d.sensors}${d.sensorErrors ? ' · ' + t('dash.sensorErrors', { n: d.sensorErrors }) : ''}`,
          ],
          [t('dash.buffered'), d.buffered === null ? '—' : d.buffered],
          [t('dash.dropped'), d.dropped === null ? '—' : d.dropped],
          [t('dash.uploadFailures'), d.uploadFailures === null ? '—' : d.uploadFailures],
          d.firmware && [t('common.firmware'), d.firmware],
          [
            t('dash.diagAge'),
            isFuture(d.t, now) ? (
              <Badge key="fut" tone="warn" title={t('dash.futureHint')}>
                {t('dash.future')}
              </Badge>
            ) : (
              t('common.ago', { age: formatDuration(Math.max(0, Math.round((now - d.t) / 1000))) })
            ),
          ],
        ]}
      />
      <DeviceCharts
        feed={{
          entries: (diag ? diag.entries : []).map((e) => ({
            t: e.t,
            values: [e.values[0] === null ? null : e.values[0] / 1000, e.values[1]],
          })),
        }}
        gapMs={gapThreshold(diag || diagLast) * 8}
        start={now - RANGES[range]}
        end={now}
      />
    </Card>
  );
}
