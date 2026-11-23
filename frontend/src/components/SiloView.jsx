import { SILO, markerLayout, spreadLabels, siloPath, grainPath } from '../lib/silo.js';
import { tempColor, pointState, formatTemp, TEMP_STOPS, TEMP_MIN, TEMP_MAX } from '../lib/temp.js';
import { useI18n, tOr } from './i18n.js';

const G = SILO;

/** Cross-section of the silo with the probe and its measuring points. */
export function SiloView({ points }) {
  const { t } = useI18n();
  const list = points && points.length ? points : [];
  const layout = markerLayout(list, G);
  const labelY = spreadLabels(
    layout.map((m) => m.y),
    17,
    G.topY - 8,
    G.wallBottomY + 18,
  );
  const byPoint = new Map(list.map((p) => [p.point, p]));
  const ribs = [];
  for (let y = G.eaveY + 24; y < G.wallBottomY; y += 24) ribs.push(y);

  return (
    <svg class="silo" viewBox={`0 0 ${G.width} ${G.height}`} role="img" aria-label={t('silo.aria')}>
      <defs>
        <pattern id="hatch" width="5" height="5" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
          <rect width="5" height="5" fill="var(--silo-err-bg)" />
          <line x1="0" y1="0" x2="0" y2="5" stroke="var(--danger)" stroke-width="2.2" />
        </pattern>
        <pattern id="grain" width="9" height="7" patternUnits="userSpaceOnUse">
          <rect width="9" height="7" fill="var(--grain)" />
          <ellipse cx="2.5" cy="2" rx="1.6" ry="1" fill="var(--grain-dot)" />
          <ellipse cx="7" cy="5.3" rx="1.6" ry="1" fill="var(--grain-dot)" />
        </pattern>
        <linearGradient id="wall" x1="0" x2="1">
          <stop offset="0" stop-color="var(--silo-wall-dark)" />
          <stop offset=".45" stop-color="var(--silo-wall)" />
          <stop offset="1" stop-color="var(--silo-wall-dark)" />
        </linearGradient>
      </defs>

      {/* legs and ground */}
      <g stroke="var(--silo-line)" stroke-width="3" stroke-linecap="round">
        <line x1={G.wallLeft + 6} y1={G.wallBottomY} x2={G.wallLeft + 6} y2={G.groundY} />
        <line x1={G.wallRight - 6} y1={G.wallBottomY} x2={G.wallRight - 6} y2={G.groundY} />
        <line x1={G.cx - 40} y1={G.wallBottomY + 30} x2={G.cx - 40} y2={G.groundY} />
        <line x1={G.cx + 40} y1={G.wallBottomY + 30} x2={G.cx + 40} y2={G.groundY} />
      </g>
      <line x1="8" y1={G.groundY} x2={G.wallRight + 20} y2={G.groundY} stroke="var(--silo-line)" stroke-width="2" />
      <rect
        x={G.cx - G.outletHalf + 4}
        y={G.hopperBottomY}
        width={(G.outletHalf - 4) * 2}
        height="12"
        rx="2"
        fill="var(--silo-line)"
      />

      {/* body */}
      <path d={siloPath(G)} fill="url(#wall)" stroke="var(--silo-line)" stroke-width="2.5" stroke-linejoin="round" />
      <path d={grainPath(G.topY + 2, G)} fill="url(#grain)" opacity=".95" />
      <g stroke="var(--silo-rib)" stroke-width="1">
        {ribs.map((y) => (
          <line key={y} x1={G.wallLeft + 1} y1={y} x2={G.wallRight - 1} y2={y} />
        ))}
      </g>
      <path
        d={`M${G.cx} ${G.roofApexY} L${G.wallRight + 8} ${G.eaveY} L${G.wallLeft - 8} ${G.eaveY} Z`}
        fill="var(--silo-roof)"
        stroke="var(--silo-line)"
        stroke-width="2.5"
        stroke-linejoin="round"
      />
      <rect x={G.cx - 9} y={G.roofApexY - 6} width="18" height="9" rx="2" fill="var(--silo-line)" />

      {/* probe */}
      <line
        x1={G.cx}
        y1={G.probeTopY}
        x2={G.cx}
        y2={G.tipY + 8}
        stroke="var(--probe)"
        stroke-width="4"
        stroke-linecap="round"
      />
      <path d={`M${G.cx - 5} ${G.tipY + 6} L${G.cx + 5} ${G.tipY + 6} L${G.cx} ${G.tipY + 16} Z`} fill="var(--probe)" />

      {/* measuring points */}
      {layout.map((m, i) => {
        const p = byPoint.get(m.point);
        const st = pointState(p);
        const color = st.valid ? tempColor(st.temp) : null;
        const ly = labelY[i];
        const lx = G.cx + 22;
        const label =
          st.state === 'ok'
            ? formatTemp(st.temp)
            : st.state === 'unmapped'
              ? t('silo.unmapped')
              : tOr(t, 'status.', p && p.status ? p.status : 'missing');
        const title =
          `${t('common.point')} ${m.point}: ${label}` +
          (p && p.rom ? ` — ROM ${p.rom}` : '') +
          (m.heightKnown ? ` — ${m.height} cm` : '');
        return (
          <g key={m.point} class={'marker marker-' + st.state}>
            <title>{title}</title>
            <polyline
              points={`${G.cx + 10},${m.y} ${lx - 6},${ly} ${lx - 2},${ly}`}
              fill="none"
              stroke="var(--silo-leader)"
              stroke-width="1"
            />
            <rect x={lx - 2} y={ly - 8} width={G.width - lx - 4} height="16" rx="8" fill="var(--silo-label-bg)" />
            <text x={lx + 5} y={ly + 4} class="silo-label">
              <tspan class="silo-pt">{m.point}</tspan>
              <tspan dx="5" class={st.state === 'error' ? 'silo-err' : ''}>
                {label}
              </tspan>
            </text>
            {m.heightKnown && (
              <text x={G.cx - 14} y={m.y + 4} text-anchor="end" class="silo-h">
                {m.height} cm
              </text>
            )}
            {st.state === 'ok' && (
              <circle cx={G.cx} cy={m.y} r="9" fill={color} stroke="var(--marker-ring)" stroke-width="2.5" />
            )}
            {st.state === 'error' && (
              <g>
                <circle cx={G.cx} cy={m.y} r="9" fill="url(#hatch)" stroke="var(--danger)" stroke-width="2.5" />
                <text x={G.cx} y={m.y + 4} text-anchor="middle" class="silo-bang">
                  !
                </text>
              </g>
            )}
            {st.state === 'unmapped' && (
              <circle
                cx={G.cx}
                cy={m.y}
                r="7"
                fill="var(--surface)"
                stroke="var(--muted)"
                stroke-width="1.8"
                stroke-dasharray="3 2.5"
              />
            )}
          </g>
        );
      })}
    </svg>
  );
}

/** Colour scale legend + symbols for error / not mapped. */
export function TempLegend() {
  const { t } = useI18n();
  const range = TEMP_MAX - TEMP_MIN;
  const stops = TEMP_STOPS.map((s) => `${tempColor(s.t)} ${(((s.t - TEMP_MIN) / range) * 100).toFixed(1)}%`).join(', ');
  const ticks = [-10, 0, 10, 20, 30, 40];
  return (
    <div class="legend">
      <div class="legend-bar" style={{ background: `linear-gradient(90deg, ${stops})` }} />
      <div class="legend-ticks">
        {ticks.map((v) => (
          <span key={v} style={{ left: (((v - TEMP_MIN) / range) * 100).toFixed(1) + '%' }}>
            {v}°
          </span>
        ))}
      </div>
      <div class="legend-keys">
        <span>
          <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true">
            <defs>
              <pattern id="hatch-l" width="4" height="4" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
                <rect width="4" height="4" fill="var(--silo-err-bg)" />
                <line x1="0" y1="0" x2="0" y2="4" stroke="var(--danger)" stroke-width="2" />
              </pattern>
            </defs>
            <circle cx="8" cy="8" r="6" fill="url(#hatch-l)" stroke="var(--danger)" stroke-width="2" />
          </svg>
          {t('silo.legendError')}
        </span>
        <span>
          <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true">
            <circle cx="8" cy="8" r="5.5" fill="none" stroke="var(--muted)" stroke-width="1.6" stroke-dasharray="3 2" />
          </svg>
          {t('silo.legendUnmapped')}
        </span>
      </div>
    </div>
  );
}
