/**
 * One request at a time, including across effect cleanup/restarts.
 * Manual refresh replaces the next cycle, or joins the request in flight.
 */
export function createPoll(fn, publish, doc = document) {
  let active = false;
  let intervalMs;
  let generation = 0;
  let timer;
  let controller;
  let pending;

  const clear = () => clearTimeout(timer);
  const schedule = () => {
    clear();
    if (active && !doc.hidden) timer = setTimeout(refresh, intervalMs);
  };
  function refresh() {
    if (pending) return pending;
    if (!active) return Promise.resolve(null);
    clear();
    const started = generation;
    controller = new AbortController();
    pending = Promise.resolve()
      .then(() => fn(controller.signal))
      .then(
        (data) => {
          if (active && started === generation) publish({ data, error: null, fetchedAt: Date.now() });
          return active && started === generation ? data : null;
        },
        (error) => {
          if (active && started === generation && error?.kind !== 'aborted') publish({ error });
          return null;
        },
      )
      .finally(() => {
        pending = null;
        if (active && started !== generation && !doc.hidden) refresh();
        else schedule();
      });
    return pending;
  }
  const visibility = () => {
    if (doc.hidden) clear();
    else refresh();
  };
  function stop() {
    active = false;
    generation++;
    clear();
    doc.removeEventListener('visibilitychange', visibility);
    controller?.abort();
  }
  function start(ms) {
    intervalMs = ms;
    active = true;
    generation++;
    doc.addEventListener('visibilitychange', visibility);
    if (!doc.hidden) refresh();
  }
  return { start, stop, refresh };
}
