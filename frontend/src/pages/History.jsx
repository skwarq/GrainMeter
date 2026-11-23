// Local history from the device flash (works without Internet at the silo):
// the same views as the remote dashboard, fed by GET /api/history/<range>.
import { useEffect, useMemo, useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n, errorText } from '../components/i18n.js';
import { useStatus } from '../components/hooks.js';
import { Card, Loading, PageHead } from '../components/ui.jsx';
import { SiloView, TempLegend } from '../components/SiloView.jsx';
import { ALERT_DEFAULTS, AlertsCard, HistoryCard, PointTable } from '../components/History.jsx';
import { describeError } from '../lib/errors.js';
import { parseHistoryBinary } from '../lib/feed.js';

const RANGE_KEY = 'gm.history.range';
const REFRESH_MS = 60000;
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
  const [buf, setBuf] = useState(null);
  const [error, setError] = useState(null);

  const setRange = (r) => {
    setRangeState(r);
    try {
      localStorage.setItem(RANGE_KEY, r);
    } catch {
      /* ignore */
    }
  };

  useEffect(() => {
    let ctl = new AbortController();
    const load = () => {
      ctl.abort();
      ctl = new AbortController();
      api
        .getHistory(range, ctl.signal)
        .then((b) => {
          setBuf(b);
          setError(null);
        })
        .catch((e) => {
          if (e && e.code === 'aborted') return;
          setError(e);
        });
    };
    load();
    const id = setInterval(() => document.visibilityState === 'visible' && load(), REFRESH_MS);
    return () => {
      clearInterval(id);
      ctl.abort();
    };
  }, [range]);

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
  const feed = useMemo(() => {
    if (!buf) return null;
    try {
      return parseHistoryBinary(buf, fields, s ? s.name : '');
    } catch {
      return null;
    }
  }, [buf, fields, s]);

  // Device clock when it is set (the phone's clock may differ).
  const now = s && s.time_valid && s.epoch ? s.epoch * 1000 : Date.now();

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
  } else if (!feed) {
    body = <Loading text={t('common.loading')} />;
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
