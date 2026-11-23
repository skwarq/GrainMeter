// Loads the device's local history day by day (local midnight to
// midnight), newest day first, page by page. Past days never change and
// are kept in a cache, so a refresh only asks for today again.
import { mergeFeeds, parseHistoryBinary } from './feed.js';

const DAY_S = 86400;
// After a power-up without Wi-Fi the device adds the measurements taken
// before its clock was set once it syncs (up to its buffer: 96 samples, 24 h
// at 15 min); days that recent are fetched again instead of cached.
export const BACKFILL_S = 2 * DAY_S;

/**
 * Day chunks [from, to] (epoch s, inclusive) covering [nowMs - rangeMs,
 * nowMs], newest first; `complete` = old enough to be cached.
 */
export function dayChunks(nowMs, rangeMs, stableAfterS = BACKFILL_S) {
  const start = new Date(nowMs - rangeMs);
  start.setHours(0, 0, 0, 0);
  const out = [];
  let d = start;
  const nowS = Math.floor(nowMs / 1000);
  while (Math.floor(d.getTime() / 1000) <= nowS) {
    const next = new Date(d);
    next.setDate(next.getDate() + 1); // DST-safe local midnight
    const from = Math.floor(d.getTime() / 1000);
    const to = Math.floor(next.getTime() / 1000) - 1;
    out.push({ from, to, complete: to < nowS - stableAfterS });
    d = next;
  }
  return out.reverse();
}

/**
 * fetchPage(from, to) -> Promise<ArrayBuffer>. onProgress(feed, done, total)
 * after every day. cache: Map shared between calls (complete days only).
 * Returns the merged feed.
 */
export async function loadHistory({ nowMs, rangeMs, fetchPage, cache, fields, name, onProgress }) {
  const chunks = dayChunks(nowMs, rangeMs);
  const days = [];
  let done = 0;
  for (const c of chunks) {
    const key = `${c.from}`;
    let day = c.complete && cache ? cache.get(key) : undefined;
    if (!day) {
      const pages = [];
      let from = c.from;
      for (let guard = 0; guard < 64; guard++) {
        const page = parseHistoryBinary(await fetchPage(from, c.to), fields, name);
        pages.push(page);
        if (!page.next || page.next <= from) break;
        from = page.next;
      }
      day = mergeFeeds(pages);
      if (c.complete && cache) cache.set(key, day);
    }
    days.push(day);
    done++;
    if (onProgress) onProgress(mergeFeeds(days, fields, name), done, chunks.length);
  }
  return mergeFeeds(days, fields, name);
}

/** Bytes per day at the current rate -> days the history can hold. */
export function historyCapacityDays(info) {
  if (!info || !info.records || !info.oldest_epoch || !info.newest_epoch) return null;
  const spanDays = (info.newest_epoch - info.oldest_epoch) / DAY_S;
  if (spanDays < 1 || !info.used_bytes) return null;
  return Math.round((info.capacity_bytes / info.used_bytes) * spanDays);
}
