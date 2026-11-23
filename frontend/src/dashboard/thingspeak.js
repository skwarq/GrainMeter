/* global __GM_API_BASE__ -- injected by scripts/build-dashboard.mjs (esbuild define) */
// The only network module of the dashboard: reads channel feeds from the
// ThingSpeak REST API directly in the browser (CORS is allowed by
// ThingSpeak). The read key is sent only to api.thingspeak.com.
import { parseFeed, thingSpeakTime } from '../lib/feed.js';

export const API_BASE = typeof __GM_API_BASE__ === 'string' ? __GM_API_BASE__ : 'https://api.thingspeak.com';
const TIMEOUT_MS = 20000;

export class FeedError extends Error {
  constructor(code, status) {
    super(code);
    this.code = code; // 'auth' | 'not_found' | 'http' | 'network' | 'timeout' | 'format'
    this.status = status;
  }
}

/** Loads entries of one channel newer than startMs. */
export async function loadFeed({ channelId, readKey, startMs }) {
  const q = new URLSearchParams({
    results: '8000',
    status: 'true',
    timezone: 'Etc/UTC',
    start: thingSpeakTime(startMs),
  });
  if (readKey) q.set('api_key', readKey);
  const url = `${API_BASE}/channels/${encodeURIComponent(channelId)}/feeds.json?${q}`;
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), TIMEOUT_MS);
  let res;
  try {
    res = await fetch(url, {
      signal: ctl.signal,
      cache: 'no-store',
      credentials: 'omit',
      referrerPolicy: 'no-referrer',
    });
  } catch (e) {
    throw new FeedError(e && e.name === 'AbortError' ? 'timeout' : 'network', 0);
  } finally {
    clearTimeout(timer);
  }
  // ThingSpeak answers a wrong key with 400/401 and a missing channel with 404.
  if (res.status === 400 || res.status === 401 || res.status === 403) throw new FeedError('auth', res.status);
  if (res.status === 404) throw new FeedError('not_found', res.status);
  if (!res.ok) throw new FeedError('http', res.status);
  let json;
  try {
    json = await res.json();
  } catch {
    throw new FeedError('format', res.status);
  }
  // A private channel without a valid key returns the number -1.
  if (json === -1 || !json || typeof json !== 'object') throw new FeedError('auth', res.status);
  return parseFeed(json);
}
