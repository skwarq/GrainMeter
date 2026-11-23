import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { downsample, niceTicks, tickDecimals } from '../lib/feed.js';

const M = { l: 46, r: 14, t: 12, b: 30 };
const DAY = 24 * 3600 * 1000;

function xTicks(start, end, lang) {
  const span = end - start;
  const step = span <= 1.5 * DAY ? 3 * 3600 * 1000 : span <= 8 * DAY ? DAY : 5 * DAY;
  const out = [];
  const d = new Date(start);
  if (step >= DAY) d.setHours(0, 0, 0, 0);
  else d.setMinutes(0, 0, 0);
  let t = d.getTime();
  while (t < start) t += step;
  const fmt =
    step >= DAY ? { day: '2-digit', month: '2-digit' } : { hour: '2-digit', minute: '2-digit', hour12: false };
  for (; t <= end; t += step) {
    out.push({ t, label: new Date(t).toLocaleString(lang === 'en' ? 'en-GB' : 'pl-PL', fmt) });
  }
  return out;
}

/**
 * Time series chart in plain SVG. series: [{key, label, color, segments}]
 * where segments come from lib/feed.js segments() (gaps = missing data).
 */
export function LineChart({ series, start, end, height = 300, unit = '°C', digits = 1, lang, emptyText }) {
  const H = height;
  const ref = useRef(null);
  const box = useRef(null);
  // Draw in real pixels so text keeps its size on phones and wide screens.
  const [W, setW] = useState(800);
  useEffect(() => {
    const el = box.current;
    if (!el || typeof ResizeObserver === 'undefined') return undefined;
    const ro = new ResizeObserver(() => setW(Math.max(280, Math.round(el.clientWidth))));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);
  const [hover, setHover] = useState(null);

  const data = useMemo(
    () => series.map((s) => ({ ...s, segments: s.segments.map((seg) => downsample(seg, 500)) })),
    [series],
  );
  const values = data.flatMap((s) => s.segments.flat().map(([, v]) => v));
  if (!values.length) {
    return (
      <div class="chart" ref={box}>
        <p class="muted chart-empty">{emptyText}</p>
      </div>
    );
  }

  let yMin = Math.min(...values);
  let yMax = Math.max(...values);
  const pad = Math.max(0.5, (yMax - yMin) * 0.08);
  yMin -= pad;
  yMax += pad;
  const ticks = niceTicks(yMin, yMax, 5);
  yMin = Math.min(yMin, ticks[0]);
  yMax = Math.max(yMax, ticks[ticks.length - 1]);

  const x = (t) => M.l + ((t - start) / (end - start || 1)) * (W - M.l - M.r);
  const y = (v) => M.t + (1 - (v - yMin) / (yMax - yMin || 1)) * (H - M.t - M.b);

  const onMove = (e) => {
    const el = ref.current;
    if (!el) return;
    const r = el.getBoundingClientRect();
    const px = ((e.clientX - r.left) / r.width) * W;
    const t = start + ((px - M.l) / (W - M.l - M.r)) * (end - start);
    if (t < start || t > end) return setHover(null);
    // nearest point of every series to t (original data, not downsampled)
    let bestT = null;
    const rows = series.map((s) => {
      let best = null;
      for (const seg of s.segments) {
        for (const p of seg) if (!best || Math.abs(p[0] - t) < Math.abs(best[0] - t)) best = p;
      }
      if (best && (bestT === null || Math.abs(best[0] - t) < Math.abs(bestT - t))) bestT = best[0];
      return { s, p: best };
    });
    if (bestT === null) return setHover(null);
    const tol = (end - start) / 60;
    setHover({
      t: bestT,
      rows: rows.map(({ s, p }) => ({ s, v: p && Math.abs(p[0] - bestT) <= tol ? p[1] : null })),
    });
  };

  // keep x labels at least ~64 px apart
  const allXt = xTicks(start, end, lang);
  const every = Math.max(1, Math.ceil((allXt.length * 64) / (W - M.l - M.r)));
  const xt = allXt.filter((_, k) => k % every === 0);
  const yDec = ticks.length > 1 ? tickDecimals(ticks[1] - ticks[0]) : 0;
  const fmtV = (v) => (v === null ? '—' : v.toFixed(digits).replace('.', lang === 'en' ? '.' : ',') + ' ' + unit);

  return (
    <div class="chart" ref={box}>
      <svg
        ref={ref}
        viewBox={`0 0 ${W} ${H}`}
        class="chart-svg"
        role="img"
        onPointerMove={onMove}
        onPointerLeave={() => setHover(null)}
      >
        {ticks.map((v) => (
          <g key={'y' + v}>
            <line class="chart-grid" x1={M.l} x2={W - M.r} y1={y(v)} y2={y(v)} />
            <text class="chart-tick" x={M.l - 6} y={y(v) + 4} text-anchor="end">
              {v.toFixed(yDec)}
            </text>
          </g>
        ))}
        {xt.map((tk) => (
          <g key={'x' + tk.t}>
            <line class="chart-grid chart-grid-v" x1={x(tk.t)} x2={x(tk.t)} y1={M.t} y2={H - M.b} />
            <text class="chart-tick" x={x(tk.t)} y={H - M.b + 18} text-anchor="middle">
              {tk.label}
            </text>
          </g>
        ))}
        {data.map((s) =>
          s.segments.map((seg, k) =>
            seg.length === 1 ? (
              <circle key={s.key + k} cx={x(seg[0][0])} cy={y(seg[0][1])} r="2.5" fill={s.color} />
            ) : (
              <path
                key={s.key + k}
                class="chart-line"
                stroke={s.color}
                d={seg.map(([t, v], j) => `${j ? 'L' : 'M'}${x(t).toFixed(1)} ${y(v).toFixed(1)}`).join('')}
              />
            ),
          ),
        )}
        {hover && (
          <g>
            <line class="chart-cursor" x1={x(hover.t)} x2={x(hover.t)} y1={M.t} y2={H - M.b} />
            {hover.rows.map(({ s, v }) =>
              v === null ? null : (
                <circle key={'h' + s.key} cx={x(hover.t)} cy={y(v)} r="4" fill={s.color} class="chart-dot" />
              ),
            )}
          </g>
        )}
      </svg>
      {hover && (
        <div class={'chart-tip' + (x(hover.t) > W / 2 ? ' left' : '')} style={{ left: (x(hover.t) / W) * 100 + '%' }}>
          <div class="chart-tip-time">
            {new Date(hover.t).toLocaleString(lang === 'en' ? 'en-GB' : 'pl-PL', {
              day: '2-digit',
              month: '2-digit',
              hour: '2-digit',
              minute: '2-digit',
              hour12: false,
            })}
          </div>
          {hover.rows.map(({ s, v }) => (
            <div key={'r' + s.key} class="chart-tip-row">
              <span class="dot" style={{ background: s.color }} />
              <span class="chart-tip-label">{s.label}</span>
              <b>{fmtV(v)}</b>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
