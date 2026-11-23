import { createContext } from 'preact';
import { useCallback, useContext, useEffect, useRef, useState } from 'preact/hooks';
import { describeError } from '../lib/errors.js';
import { useI18n, errorText } from './i18n.js';

export const StatusContext = createContext(null);
/** {status, error, fetchedAt, refresh, setStatus} shared by all pages. */
export const useStatus = () => useContext(StatusContext);

/**
 * Polls `fn(signal)` every `intervalMs` while the tab is visible.
 * Requests never overlap; a hidden tab pauses polling, becoming visible
 * triggers an immediate refresh.
 */
export function usePoll(fn, intervalMs, enabled = true) {
  const [state, setState] = useState({ data: null, error: null, fetchedAt: 0 });
  const fnRef = useRef(fn);
  fnRef.current = fn;
  const busy = useRef(false);
  const timer = useRef(0);
  const ctrl = useRef(null);

  const run = useCallback(async () => {
    if (busy.current) return;
    busy.current = true;
    clearTimeout(timer.current);
    ctrl.current = new AbortController();
    try {
      const data = await fnRef.current(ctrl.current.signal);
      setState({ data, error: null, fetchedAt: Date.now() });
    } catch (e) {
      if (e && e.kind !== 'aborted') setState((s) => ({ ...s, error: e }));
    } finally {
      busy.current = false;
      if (enabled && !document.hidden) timer.current = setTimeout(run, intervalMs);
    }
  }, [intervalMs, enabled]);

  useEffect(() => {
    if (!enabled) return undefined;
    const onVis = () => {
      if (document.hidden) clearTimeout(timer.current);
      else run();
    };
    document.addEventListener('visibilitychange', onVis);
    run();
    return () => {
      document.removeEventListener('visibilitychange', onVis);
      clearTimeout(timer.current);
      if (ctrl.current) ctrl.current.abort();
    };
  }, [run, enabled]);

  const set = useCallback((data) => setState({ data, error: null, fetchedAt: Date.now() }), []);
  return { ...state, refresh: run, set };
}

/** One-shot loader with manual reload. */
export function useLoad(fn, deps = []) {
  const [state, setState] = useState({ data: null, error: null, loading: true });
  const load = useCallback(async () => {
    setState((s) => ({ ...s, loading: true }));
    try {
      const data = await fn();
      setState({ data, error: null, loading: false });
      return data;
    } catch (e) {
      setState((s) => ({ ...s, error: e, loading: false }));
      return null;
    }
  }, deps);
  useEffect(() => {
    load();
  }, [load]);
  const set = useCallback((data) => setState({ data, error: null, loading: false }), []);
  return { ...state, reload: load, set };
}

/**
 * Wraps an async action: busy flag, success/error message for <Result>.
 * The password dialog being cancelled is not reported as an error.
 */
export function useAction() {
  const { t } = useI18n();
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState(null);
  const mounted = useRef(true);
  useEffect(() => () => (mounted.current = false), []);
  const run = useCallback(
    async (fn, { ok, context } = {}) => {
      setBusy(true);
      setResult(null);
      try {
        const r = await fn();
        if (mounted.current) {
          const msg = typeof ok === 'function' ? ok(r) : ok;
          if (msg) setResult({ type: 'ok', text: msg });
        }
        return r;
      } catch (e) {
        if (mounted.current && !(e && e.kind === 'aborted')) {
          setResult({ type: 'error', text: errorText(t, describeError(e, context)) });
        }
        return undefined;
      } finally {
        if (mounted.current) setBusy(false);
      }
    },
    [t],
  );
  return { busy, result, run, setResult };
}

/** Re-renders every `ms` (for countdowns / ages between polls). */
export function useTick(ms = 1000) {
  const [, set] = useState(0);
  useEffect(() => {
    const id = setInterval(() => set((n) => n + 1), ms);
    return () => clearInterval(id);
  }, [ms]);
}

/** Current hash route ("#/sensors" -> "sensors"). */
export function useHashRoute() {
  const get = () => (location.hash.replace(/^#\/?/, '').split(/[?/]/)[0] || 'overview').toLowerCase();
  const [route, setRoute] = useState(get);
  useEffect(() => {
    const on = () => setRoute(get());
    window.addEventListener('hashchange', on);
    return () => window.removeEventListener('hashchange', on);
  }, []);
  return route;
}
