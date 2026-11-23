// Local history from the device flash (works without Internet at the silo):
// the same views as the remote dashboard, fed by GET /api/history/<range>.
import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n, errorText } from '../components/i18n.js';
import { useStatus } from '../components/hooks.js';
import { Card, KV, Loading, PageHead } from '../components/ui.jsx';
import { SiloView, TempLegend } from '../components/SiloView.jsx';
import { ALERT_DEFAULTS, AlertsCard, DeviceCharts, HistoryCard, PointTable } from '../components/History.jsx';
import { describeError } from '../lib/errors.js';
import { RANGES, deviceFeed, deviceSummary } from '../lib/feed.js';
import { historyCapacityDays, loadHistory } from '../lib/historyLoader.js';
import { formatBytes, rssiPercent } from '../lib/format.js';
import { batteryPercent } from '../lib/battery.js';

const RANGE_KEY = 'gm.history.range';
const REFRESH_MS = 60000;
// Complete past days, kept while the portal is open (they never change).
const dayCache = new Map();
// No "stale" alert here: the portal shows live device data next to it.
const SETTINGS = { ...ALERT_DEFAULTS, staleMinutes: 0 };

function initialRange() {
  try {
    const r = localStorage.getItem(RANGE_KEY);
    if (r === '24h' || r === '7d' || r === '30d') return r;
  } catch {
    /* ignore */
  }
  return '7d';
}

export function History() {
  const { t } = useI18n();
  const { status: s } = useStatus();
  const [range, setRangeState] = useState(initialRange);
  const [feed, setFeed] = useState(null);
  const [progress, setProgress] = useState(null);
  const [info, setInfo] = useState(null);
  const [error, setError] = useState(null);

  const setRange = (r) => {
    setRangeState(r);
    setFeed(null);
    try {
      localStorage.setItem(RANGE_KEY, r);
    } catch {
      /* ignore */
    }
  };

  const points = (s && s.measurement && s.measurement.points) || [];
  const heightsKey = points.map((p) => p.height_cm).join(',');
  const fields = useMemo(
    () =>
      Array.from({ length: 8 }, (_, i) => {
        const p = points[i] || {};
        const h = typeof p.height_cm === 'number' && p.height_cm >= 0 ? p.height_cm : null;
        return {
          label: t('dash.point', { n: i + 1 }) + (h !== null ? ` (${h} cm)` : ''),
          heightCm: h,
        };
      }),
    [heightsKey, t],
  );
  const deviceNow = s && s.time_valid && s.epoch ? s.epoch * 1000 : null;
  const deviceNowRef = useRef(deviceNow);
  deviceNowRef.current = deviceNow;

  useEffect(() => {
    let ctl = new AbortController();
    let busy = false;
    const load = async () => {
      if (busy) return;
      busy = true;
      ctl.abort();
      ctl = new AbortController();
      const signal = ctl.signal;
      try {
        const inf = await api.getHistoryInfo(signal);
        setInfo(inf);
        if (!inf.available) {
          setError({ status: 503 });
          return;
        }
        // Device clock when it is set (the phone's clock may differ).
        const nowMs = deviceNowRef.current || Date.now();
        const f = await loadHistory({
          nowMs,
          rangeMs: RANGES[range],
          fetchPage: (from, to) => api.getHistoryPage(from, to, signal),
          cache: dayCache,
          fields,
          onProgress: (partial, done, total) => {
            if (signal.aborted) return;
            setFeed(partial);
            setProgress(done < total ? { done, total } : null);
          },
        });
        if (!signal.aborted) {
          setFeed(f);
          setProgress(null);
          setError(null);
        }
      } catch (e) {
        if (!(e && e.code === 'aborted') && !signal.aborted) setError(e);
      } finally {
        busy = false;
      }
    };
    load();
    const id = setInterval(() => document.visibilityState === 'visible' && load(), REFRESH_MS);
    return () => {
      clearInterval(id);
      ctl.abort();
    };
  }, [range, fields]);

  const now = deviceNow || Date.now();

  let body;
  if (error) {
    const unavailable = error.status === 503;
    body = (
      <Card>
        <p class={unavailable ? 'muted' : 'result result-error'}>
          {unavailable ? t('hist.unavailable') : errorText(t, describeError(error))}
        </p>
      </Card>
    );
  } else if (!feed || (feed.entries.length === 0 && progress)) {
    body = <Loading text={progress ? t('hist.loadingDays', progress) : t('common.loading')} />;
  } else if (feed.entries.length === 0) {
    body = (
      <Card>
        <p class="muted">
          {s && !s.time_valid ? t('hist.noClock') : t('dash.noData', { range: t('dash.range.' + range) })}
        </p>
      </Card>
    );
  } else {
    body = (
      <>
        {progress && <p class="muted small">{t('hist.loadingDays', progress)}</p>}
        <AlertsCard feed={feed} settings={SETTINGS} now={now} />
        <div class="dash-grid">
          <HistoryCard feed={feed} range={range} onRange={setRange} now={now} />
          <Card title={t('dash.siloNow')} icon="overview" class="dash-silo span-all">
            <div class="silo-wrap">
              <div>
                <SiloView points={points} />
                <TempLegend />
              </div>
              <PointTable feed={feed} settings={SETTINGS} points={points} />
            </div>
          </Card>
          <DeviceHistoryCard feed={feed} range={range} now={now} info={info} />
        </div>
      </>
    );
  }

  return (
    <div class="page-history">
      <PageHead title={t('nav.history')} sub={t('hist.sub')} />
      {body}
    </div>
  );
}

const volts = (mv) => (mv / 1000).toFixed(2) + ' V';
const voltsPct = (mv) => {
  const p = batteryPercent(mv);
  return volts(mv) + (p === null ? '' : ` · ~${p} %`);
};
const dbm = (r) => {
  const p = rssiPercent(r);
  return `${r} dBm` + (p === null ? '' : ` · ${p} %`);
};

function DeviceHistoryCard({ feed, range, now, info }) {
  const { t } = useI18n();
  const sum = useMemo(() => deviceSummary(feed), [feed]);
  const dev = useMemo(() => deviceFeed(feed), [feed]);
  if (!sum) return null;
  const capDays = historyCapacityDays(info);
  const storage =
    info && info.capacity_bytes
      ? `${formatBytes(info.used_bytes)} / ${formatBytes(info.capacity_bytes)}` +
        (capDays ? ' · ' + t('hist.capacityDays', { n: capDays }) : '')
      : '—';
  const pct = (v) => (v === null ? '—' : v + ' %');
  return (
    <Card title={t('dash.device')} icon="chip" class="span-all dash-device-history">
      <p class="muted small">{t('hist.deviceSub')}</p>
      <div class="device-history">
        <KV
          items={[
            [t('overview.battery'), sum.batteryLast === null ? '—' : voltsPct(sum.batteryLast)],
            [
              t('hist.batteryRange'),
              sum.batteryMin === null ? '—' : `${volts(sum.batteryMin)} – ${volts(sum.batteryMax)}`,
            ],
            [t('hist.signal'), sum.rssiLast === null ? '—' : dbm(sum.rssiLast)],
            [t('hist.weakestSignal'), sum.rssiWeakest === null ? '—' : dbm(sum.rssiWeakest)],
            [t('hist.failedUploads'), sum.failedUploads],
            [t('hist.powerTime'), `${pct(sum.economyPct)} / ${pct(sum.criticalPct)}`],
            [t('hist.storage'), storage],
            info && [t('hist.records'), info.records],
          ]}
        />
        <DeviceCharts feed={dev} start={now - RANGES[range]} end={now} />
      </div>
    </Card>
  );
}
