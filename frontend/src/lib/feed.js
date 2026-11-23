// ThingSpeak channel feeds -> data for the remote dashboard. Pure functions
// (no DOM, no network) so they are unit-tested in Node.
//
// Main channel (written by the device): field1..field8 = temperature of
// measuring point 1..8 [degC], missing value = field absent/null (never 0),
// status = "seq=123 p3:crc_error p5:missing".
// Diagnostic channel: field1 battery mV, 2 RSSI dBm, 3 sensors found,
// 4 sensor errors, 5 buffered samples, 6 dropped, 7 upload failures,
// 8 wake count; status = "fw=0.1.1 wake=timer power=normal".

export const FIELD_COUNT = 8;
const HOUR = 3600 * 1000;
const DAY = 24 * HOUR;

export const RANGES = { '24h': DAY, '7d': 7 * DAY, '30d': 30 * DAY };

/** Number from a ThingSpeak field value, or null (missing / not numeric). */
export function toNumber(v) {
  if (v === null || v === undefined) return null;
  const s = String(v).trim();
  if (s === '') return null;
  const n = Number(s);
  return Number.isFinite(n) ? n : null;
}

/** Parses the device status text: { seq, errors: {point: reason}, kv }. */
export function parseStatus(text) {
  const out = { seq: null, errors: {}, kv: {} };
  if (typeof text !== 'string') return out;
  for (const tok of text.trim().split(/\s+/)) {
    let m = /^p([1-8]):([a-z_]+)$/.exec(tok);
    if (m) {
      out.errors[Number(m[1])] = m[2];
      continue;
    }
    m = /^([a-z_]+)=(\S+)$/.exec(tok);
    if (m) {
      out.kv[m[1]] = m[2];
      if (m[1] === 'seq') out.seq = Number(m[2]);
    }
  }
  return out;
}

/** "Punkt 3 (450 cm)" -> 450; "P2 1,5 m" -> 150; no height -> null. */
export function heightFromLabel(label) {
  if (typeof label !== 'string') return null;
  const m = /(\d+(?:[.,]\d+)?)\s*(cm|m)\b/i.exec(label);
  if (!m) return null;
  const v = Number(m[1].replace(',', '.'));
  if (!Number.isFinite(v)) return null;
  return Math.round(m[2].toLowerCase() === 'm' ? v * 100 : v);
}

/**
 * Normalises a /channels/<id>/feeds.json response:
 * { name, fields: [{index, label, heightCm}], entries: [{t, values[8], errors, kv}] }
 * Entries are sorted by time; unparsable timestamps are dropped.
 */
export function parseFeed(json) {
  const ch = (json && json.channel) || {};
  const fields = [];
  for (let i = 1; i <= FIELD_COUNT; i++) {
    const label = typeof ch['field' + i] === 'string' ? ch['field' + i] : '';
    fields.push({ index: i, label, heightCm: heightFromLabel(label) });
  }
  const entries = [];
  for (const f of (json && Array.isArray(json.feeds) && json.feeds) || []) {
    const t = Date.parse(f.created_at);
    if (!Number.isFinite(t)) continue;
    const values = [];
    for (let i = 1; i <= FIELD_COUNT; i++) values.push(toNumber(f['field' + i]));
    const st = parseStatus(f.status);
    entries.push({ t, values, errors: st.errors, kv: st.kv });
  }
  entries.sort((a, b) => a.t - b.t);
  return { name: typeof ch.name === 'string' ? ch.name : '', fields, entries };
}

/** Index of the last entry, or -1. */
const last = (feed) => feed.entries.length - 1;

/** Whether point i (0-based) ever had a value in the loaded range. */
export function hasData(feed, i) {
  return feed.entries.some((e) => e.values[i] !== null);
}

/**
 * Points for the silo view (same shape as the device API):
 * {point, rom, height_cm, status, temp_c} from the newest entry.
 */
export function latestPoints(feed) {
  const e = feed.entries[last(feed)];
  return feed.fields.map((f, i) => {
    const v = e ? e.values[i] : null;
    let status = 'ok';
    if (v === null) {
      if (e && e.errors[i + 1]) status = e.errors[i + 1];
      else status = hasData(feed, i) ? 'missing' : 'not_mapped';
    }
    return {
      point: i + 1,
      // the silo view treats a point without "rom" as not mapped
      rom: status === 'not_mapped' ? null : f.label || 'field' + (i + 1),
      height_cm: f.heightCm,
      status,
      temp_c: v,
    };
  });
}

/** min / max / avg / last / count of point i over all loaded entries. */
export function pointStats(feed, i) {
  let min = Infinity;
  let max = -Infinity;
  let sum = 0;
  let count = 0;
  let lastValue = null;
  for (const e of feed.entries) {
    const v = e.values[i];
    if (v === null) continue;
    min = Math.min(min, v);
    max = Math.max(max, v);
    sum += v;
    count++;
    lastValue = v;
  }
  if (count === 0) return { min: null, max: null, avg: null, last: null, count: 0 };
  return { min, max, avg: sum / count, last: lastValue, count };
}

/**
 * Temperature change of point i over the last 24 h: newest value minus the
 * value closest to 24 h earlier (within +-tolerance). null when unknown.
 */
export function change24h(feed, i, toleranceMs = 3 * HOUR) {
  let newest = null;
  for (let k = last(feed); k >= 0; k--) {
    if (feed.entries[k].values[i] !== null) {
      newest = feed.entries[k];
      break;
    }
  }
  if (!newest) return null;
  const target = newest.t - DAY;
  let best = null;
  for (const e of feed.entries) {
    if (e.values[i] === null) continue;
    const d = Math.abs(e.t - target);
    if (d <= toleranceMs && (!best || d < Math.abs(best.t - target))) best = e;
  }
  return best ? newest.values[i] - best.values[i] : null;
}

/** Typical spacing between entries (median), ms; null with < 2 entries. */
export function typicalInterval(feed) {
  const d = [];
  for (let k = 1; k < feed.entries.length; k++) d.push(feed.entries[k].t - feed.entries[k - 1].t);
  if (!d.length) return null;
  d.sort((a, b) => a - b);
  return d[Math.floor(d.length / 2)];
}

/**
 * Alerts for the dashboard. settings: {maxTemp, risePerDay, staleMinutes}
 * (null/0 disables a rule). Returns [{kind, point?, value?}] with kind
 * 'stale' | 'hot' | 'rising' | 'sensor'.
 */
export function alerts(feed, settings, now = Date.now()) {
  const out = [];
  const e = feed.entries[last(feed)];
  if (!e) return out;
  const staleMs = (settings.staleMinutes || 0) * 60 * 1000;
  if (staleMs > 0 && now - e.t > staleMs) out.push({ kind: 'stale', value: Math.round((now - e.t) / 60000) });
  for (let i = 0; i < FIELD_COUNT; i++) {
    const v = e.values[i];
    if (v === null) {
      if (e.errors[i + 1] && e.errors[i + 1] !== 'not_mapped') {
        out.push({ kind: 'sensor', point: i + 1, value: e.errors[i + 1] });
      }
      continue;
    }
    if (
      settings.maxTemp !== null &&
      settings.maxTemp !== undefined &&
      settings.maxTemp !== '' &&
      v >= Number(settings.maxTemp)
    ) {
      out.push({ kind: 'hot', point: i + 1, value: v });
    }
    const rise = change24h(feed, i);
    if (settings.risePerDay && rise !== null && rise >= Number(settings.risePerDay)) {
      out.push({ kind: 'rising', point: i + 1, value: rise });
    }
  }
  return out;
}

/**
 * Line segments of point i for the chart: [[[t, v], ...], ...]. A segment
 * ends at a missing value or a time gap larger than gapMs, so outages are
 * drawn as gaps and never as a line through 0.
 */
export function segments(feed, i, gapMs) {
  const out = [];
  let cur = [];
  let prevT = null;
  for (const e of feed.entries) {
    const v = e.values[i];
    if (v === null || (prevT !== null && e.t - prevT > gapMs)) {
      if (cur.length) out.push(cur);
      cur = [];
    }
    if (v !== null) cur.push([e.t, v]);
    prevT = e.t;
  }
  if (cur.length) out.push(cur);
  return out;
}

/** Gap threshold for segments(): 3x the typical interval, at least 45 min. */
export function gapThreshold(feed) {
  const iv = typicalInterval(feed);
  return Math.max(45 * 60 * 1000, iv ? iv * 3 : 0);
}

/**
 * Reduces a segment to at most maxPoints by averaging equal time buckets
 * (keeps the chart fast for 30 days x 8 points).
 */
export function downsample(seg, maxPoints) {
  if (seg.length <= maxPoints) return seg;
  const t0 = seg[0][0];
  const span = seg[seg.length - 1][0] - t0 || 1;
  const buckets = new Map();
  for (const [t, v] of seg) {
    const b = Math.min(maxPoints - 1, Math.floor(((t - t0) / span) * maxPoints));
    const acc = buckets.get(b) || [0, 0, 0];
    acc[0] += t;
    acc[1] += v;
    acc[2]++;
    buckets.set(b, acc);
  }
  return [...buckets.keys()]
    .sort((a, b) => a - b)
    .map((b) => {
      const [st, sv, n] = buckets.get(b);
      return [st / n, sv / n];
    });
}

/** "Nice" axis ticks covering [min, max] with about `count` steps. */
export function niceTicks(min, max, count = 5) {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return [];
  if (min === max) {
    min -= 1;
    max += 1;
  }
  const raw = (max - min) / count;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
  const dec = tickDecimals(step);
  const ticks = [];
  for (let k = Math.floor(min / step); k * step <= max + step * 1e-9; k++) {
    ticks.push(Number((k * step).toFixed(dec))); // no 3.4000000000000004
  }
  if (ticks[ticks.length - 1] < max) ticks.push(Number((ticks[ticks.length - 1] + step).toFixed(dec)));
  return ticks;
}

/** Decimals needed to print ticks spaced by `step` (0.2 -> 1, 5 -> 0). */
export function tickDecimals(step) {
  return Math.max(0, -Math.floor(Math.log10(step) + 1e-9));
}

/** ThingSpeak "start" parameter (UTC, used with timezone=Etc/UTC). */
export function thingSpeakTime(ms) {
  const d = new Date(ms);
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getUTCFullYear()}-${p(d.getUTCMonth() + 1)}-${p(d.getUTCDate())} ${p(d.getUTCHours())}:${p(d.getUTCMinutes())}:${p(d.getUTCSeconds())}`;
}

/** Latest diagnostic values from the diagnostic channel feed (or null). */
export function latestDiag(feed) {
  const e = feed && feed.entries[last(feed)];
  if (!e) return null;
  const [batteryMv, rssi, sensors, sensorErrors, buffered, dropped, uploadFailures, wakeCount] = e.values;
  return {
    t: e.t,
    batteryMv,
    rssi,
    sensors,
    sensorErrors,
    buffered,
    dropped,
    uploadFailures,
    wakeCount,
    firmware: e.kv.fw || null,
    wake: e.kv.wake || null,
    power: e.kv.power || null,
  };
}
