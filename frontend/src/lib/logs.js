export const LOG_LIMIT = 500;
export const LOG_LEVELS = ['E', 'W', 'I', 'D'];

/* eslint-disable no-control-regex -- Strip terminal escape sequences and controls from untrusted logs. */
export function plainLog(text) {
  return String(text)
    .replace(/\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g, '')
    .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '')
    .replace(/\r\n?/g, '\n')
    .replace(/[\x00-\x08\x0b-\x1f\x7f]/g, '');
}
/* eslint-enable no-control-regex */

export function logLevel(text) {
  return /(?:^|\s|\])([EWID])\s+\((?:\d+|\d{2}:\d{2}:\d{2}\.\d{3})\)\s/.exec(text)?.[1] || null;
}

export class RemoteLogs {
  constructor() {
    this.lines = [];
    this.paused = false;
    this.state = 'idle';
    this.listeners = new Set();
  }
  subscribe(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
  notify() {
    for (const listener of this.listeners) listener();
  }
  setState(state) {
    this.state = state;
    this.notify();
  }
  append(payload) {
    if (this.paused) return;
    const lines = plainLog(payload).split('\n');
    if (lines.at(-1) === '') lines.pop();
    const incoming = lines.slice(-LOG_LIMIT).map((text) => ({ text, level: logLevel(text) }));
    this.lines = [...this.lines, ...incoming].slice(-LOG_LIMIT);
    this.notify();
  }
  pause(paused) {
    this.paused = paused;
    this.notify();
  }
  clear() {
    this.lines = [];
    this.notify();
  }
  visible(levels = LOG_LEVELS) {
    return this.lines.filter((line) => line.level === null || levels.includes(line.level));
  }
  text(levels = LOG_LEVELS) {
    const lines = this.visible(levels);
    return lines.length ? lines.map((line) => line.text).join('\n') + '\n' : '';
  }
}

export class PortalLogs extends RemoteLogs {
  constructor(read, timers = globalThis) {
    super();
    this.read = read;
    this.timers = timers;
    this.next = 0;
    this.lost = 0;
    this.open = false;
    this.generation = 0;
    this.error = null;
  }
  stop() {
    ++this.generation;
    this.timers.clearTimeout(this.timer);
    this.controller?.abort();
  }
  setOpen(open) {
    this.open = open;
    this.stop();
    if (open && !this.paused) this.poll(this.generation);
  }
  pause(paused) {
    super.pause(paused);
    this.setOpen(this.open);
  }
  clear() {
    this.lost = 0;
    super.clear();
  }
  async poll(generation) {
    this.controller = new AbortController();
    try {
      const page = await this.read(this.next, this.controller.signal);
      if (generation !== this.generation) return;
      if (
        !Number.isSafeInteger(page.next) ||
        page.next < 0 ||
        !Number.isSafeInteger(page.lost) ||
        page.lost < 0 ||
        !Array.isArray(page.lines) ||
        !page.lines.every((line) => typeof line === 'string')
      ) {
        throw new Error('Invalid log response');
      }
      this.next = page.next;
      this.lost += page.lost;
      this.error = null;
      for (const line of page.lines) this.append(line + '\n');
      this.notify();
    } catch (error) {
      if (generation !== this.generation) return;
      this.error = error;
      this.notify();
    }
    if (generation === this.generation) {
      this.timer = this.timers.setTimeout(() => this.poll(generation), 2000);
    }
  }
}

const messages = {
  en: {
    title: 'Logs',
    pause: 'Pause',
    resume: 'Resume',
    clear: 'Clear',
    download: 'Download .txt',
    levels: 'Log levels',
    empty: 'No log lines.',
    paused: 'Paused: new lines are discarded.',
    subscribing: 'Subscribing to logs…',
    denied: 'Log access denied by the broker. Allow Subscribe on gm/<id>/log; the remote tunnel still works.',
    error: 'Log subscription failed. The remote tunnel still works; reconnect to retry.',
    offline: 'Broker disconnected.',
    hint: 'Untrusted plain text · last 500 lines · unclassified lines stay visible.',
    previewPaused: 'Paused: polling stopped.',
    lost: 'Lost lines:',
    pollingError: 'Log preview request failed:',
    configHint: 'Device verbosity: log_level in Configuration (Info when remote logging is disabled).',
  },
  pl: {
    title: 'Logi',
    pause: 'Wstrzymaj',
    resume: 'Wznów',
    clear: 'Wyczyść',
    download: 'Pobierz .txt',
    levels: 'Poziomy logów',
    empty: 'Brak logów.',
    paused: 'Wstrzymano: nowe linie są odrzucane.',
    subscribing: 'Subskrybowanie logów…',
    denied: 'Broker odmówił dostępu do logów. Zezwól na Subscribe gm/<id>/log; zdalny tunel nadal działa.',
    error: 'Subskrypcja logów nie powiodła się. Zdalny tunel nadal działa; połącz ponownie, aby spróbować.',
    offline: 'Broker rozłączony.',
    hint: 'Niezaufany zwykły tekst · ostatnie 500 linii · linie bez poziomu pozostają widoczne.',
    previewPaused: 'Wstrzymano odpytywanie.',
    lost: 'Utracone linie:',
    pollingError: 'Błąd odczytu podglądu logów:',
    configHint: 'Poziom urządzenia: log_level w Konfiguracji (Info przy wyłączonych logach zdalnych).',
  },
};

export function logText(lang, key) {
  return (messages[lang] || messages.en)[key] || '';
}
