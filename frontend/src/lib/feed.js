// ThingSpeak channel feeds -> data for the remote dashboard. Pure functions
// (no DOM, no network) so they are unit-tested in Node.
//
// Main channel (written by the device): field1..field8 = temperature of
// measuring point 1..8 [degC], missing value = field absent/null (never 0),
// status = "seq=123 p3:crc_error p5:missing".
// Diagnostic channel: field1 battery mV, 2 RSSI dBm, 3 sensors found,
// 4 sensor errors, 5 buffered samples, 6 dropped, 7 upload failures,
// 8 wake count; status = "fw=0.1.1 wake=timer power=normal".

import { batteryPercent, batteryRuntimeDays } from './battery.js';
import { rssiPercent } from './format.js';

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
 * ends at a missing value or a time gap larger than gapMs. With skipMissing,
 * absent fields are skipped and gaps are measured between actual values.
 * Device history retains its missing-value and restart boundaries.
 */
export function segments(feed, i, gapMs, skipMissing = false) {
  const out = [];
  let cur = [];
  let prevT = null;
  for (const e of feed.entries) {
    const v = e.values[i];
    if ((!skipMissing && v === null) || e.restart || (prevT !== null && e.t - prevT > gapMs)) {
      if (cur.length) out.push(cur);
      cur = [];
    }
    if (v !== null) cur.push([e.t, v]);
    if (!skipMissing || v !== null) prevT = e.t;
  }
  if (cur.length) out.push(cur);
  return out;
}

/**
 * Gap threshold for segments(): the device's own limit for its change-only
 * history, else 3x the typical interval, at least 45 min.
 */
export function gapThreshold(feed) {
  if (feed.maxGapMs) return feed.maxGapMs; // device history: changes only
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
  // Battery-only rows arrive between complete diagnostics. Use the newest
  // complete row, even if it reports unknown consumption after a reset.
  let energy = e;
  for (let i = last(feed); i >= 0; i--) {
    const row = feed.entries[i];
    if (row.values[2] != null || Object.keys(row.kv).length) {
      energy = row;
      break;
    }
  }
  const mah = toNumber(energy.kv.mah);
  const capacity = energy.kv.capacity === undefined ? 2500 : toNumber(energy.kv.capacity);
  const rt = toNumber(energy.kv.rt);
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
    firmware: energy.kv.fw || null,
    wake: energy.kv.wake || null,
    power: energy.kv.power || null,
    uploadS: toNumber(energy.kv.up),
    measureS: toNumber(energy.kv.ms),
    lanWake: energy.kv.lan === '1' ? true : energy.kv.lan === '0' ? false : null,
    talkback: energy.kv.tb === '1' ? true : energy.kv.tb === '0' ? false : null,
    batteryRuntime: {
      mah_per_day: mah,
      days: rt !== null && rt >= 0 ? rt : batteryRuntimeDays(mah, energy.values[0], capacity),
    },
  };
}

const POWER_MODES = ['normal', 'economy', 'critical', null];
const UPLOAD_STATES = [null, 'ok', 'failed', null];
const HEADER = 16;
const ROW = 24;

/**
 * One page of the device's local history (GET /api/history/<from>/<to>):
 * header "GMH3", uint16 rows, uint16 points(8), uint32 next (0 = complete,
 * else ask again from there), uint32 maxGap s; rows of 24 B: uint32 epoch,
 * int16 centi[8] (INT16_MIN = missing), uint16 battery mV (0 = unknown),
 * int8 RSSI dBm (0 = unknown), flags (bits 0-1 power mode, 2-3 last upload,
 * 4 reconstructed time, 5 restart after power-up, 6 held until now).
 * Rows are changes only (unchanged measurements are not stored): draw
 * lines between them; rows further apart than maxGap = no data.
 * Returns the same shape as parseFeed() plus `next` and `maxGapMs`.
 * fields: [{label, heightCm}] for the 8 points (from /api/status).
 */
export function parseHistoryBinary(buffer, fields = [], name = '') {
  const dv = new DataView(buffer);
  const magic =
    dv.byteLength >= HEADER ? String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3)) : '';
  if (magic !== 'GMH3') throw new Error('bad history format');
  const rows = dv.getUint16(4, true);
  const points = dv.getUint16(6, true);
  if (points !== FIELD_COUNT || dv.byteLength < HEADER + rows * ROW) throw new Error('bad history format');
  const entries = [];
  for (let k = 0; k < rows; k++) {
    const o = HEADER + k * ROW;
    const values = [];
    for (let i = 0; i < FIELD_COUNT; i++) {
      const c = dv.getInt16(o + 4 + 2 * i, true);
      values.push(c === -32768 ? null : c / 100);
    }
    const mv = dv.getUint16(o + 20, true);
    const rssi = dv.getInt8(o + 22);
    const flags = dv.getUint8(o + 23);
    entries.push({
      t: dv.getUint32(o, true) * 1000,
      values,
      errors: {},
      kv: {},
      restart: Boolean(flags & 0x20),
      diag: {
        batteryMv: mv || null,
        rssi: rssi || null,
        power: POWER_MODES[flags & 3],
        upload: UPLOAD_STATES[(flags >> 2) & 3],
        reconstructed: Boolean(flags & 0x10),
        hold: Boolean(flags & 0x40),
      },
    });
  }
  entries.sort((a, b) => a.t - b.t);
  return {
    name,
    fields: historyFields(fields),
    entries,
    next: dv.getUint32(8, true),
    maxGapMs: dv.getUint32(12, true) * 1000,
  };
}

function historyFields(fields) {
  const f = [];
  for (let i = 0; i < FIELD_COUNT; i++) {
    const src = fields[i] || {};
    f.push({ index: i + 1, label: src.label || '', heightCm: src.heightCm === undefined ? null : src.heightCm });
  }
  return f;
}

/**
 * Joins history pages/days into one feed (sorted, one entry per time; a
 * held "until now" row is dropped when a real row follows it).
 */
export function mergeFeeds(feeds, fields = [], name = '') {
  const all = feeds.flatMap((f) => f.entries).sort((a, b) => a.t - b.t);
  const entries = [];
  for (const e of all) {
    const prev = entries[entries.length - 1];
    if (prev && prev.t === e.t) continue;
    if (prev && prev.diag && prev.diag.hold) entries.pop();
    entries.push(e);
  }
  const maxGapMs = Math.max(0, ...feeds.map((f) => f.maxGapMs || 0)) || undefined;
  return { name, fields: historyFields(fields), entries, maxGapMs };
}

/** Device state over time as a feed: values [battery V, RSSI dBm]. */
export function deviceFeed(feed) {
  return {
    entries: feed.entries
      .filter((e) => e.diag)
      .map((e) => ({
        t: e.t,
        restart: e.restart,
        values: [e.diag.batteryMv === null ? null : e.diag.batteryMv / 1000, e.diag.rssi],
      })),
    maxGapMs: feed.maxGapMs,
  };
}

/**
 * Summary of the device state stored with the local history (null without
 * device data). Times in power modes are weighted by how long each row
 * lasted (rows are changes only); failedUploads counts periods in which the
 * last upload had failed.
 */
export function deviceSummary(feed) {
  const rows = feed.entries.filter((e) => e.diag);
  if (!rows.length) return null;
  const gap = gapThreshold(feed);
  let bMin = null;
  let bMax = null;
  let rMin = null;
  let failed = 0;
  let known = 0;
  let economy = 0;
  let critical = 0;
  let lastBattery = null;
  let lastRssi = null;
  let prevUpload = null;
  rows.forEach((e, k) => {
    const d = e.diag;
    if (d.batteryMv !== null) {
      bMin = bMin === null ? d.batteryMv : Math.min(bMin, d.batteryMv);
      bMax = bMax === null ? d.batteryMv : Math.max(bMax, d.batteryMv);
      lastBattery = d.batteryMv;
    }
    if (d.rssi !== null) {
      rMin = rMin === null ? d.rssi : Math.min(rMin, d.rssi);
      lastRssi = d.rssi;
    }
    if (d.upload === 'failed' && prevUpload !== 'failed') failed++;
    if (d.upload) prevUpload = d.upload;
    const next = rows[k + 1];
    const dt = next && !next.restart ? Math.min(next.t - e.t, gap) : 0;
    if (d.power && dt > 0) {
      known += dt;
      if (d.power === 'economy') economy += dt;
      if (d.power === 'critical') critical += dt;
    }
  });
  const pct = (n) => (known ? Math.round((n * 100) / known) : null);
  return {
    batteryMin: bMin,
    batteryMax: bMax,
    batteryLast: lastBattery,
    rssiWeakest: rMin,
    rssiLast: lastRssi,
    failedUploads: failed,
    economyPct: pct(economy),
    criticalPct: pct(critical),
  };
}

/** Feed with values [battery V, RSSI dBm] -> [battery %, signal %]. */
export function devicePercentFeed(feed) {
  return {
    ...feed,
    entries: feed.entries.map((e) => ({
      ...e,
      values: [e.values[0] === null ? null : batteryPercent(e.values[0] * 1000), rssiPercent(e.values[1])],
    })),
  };
}
