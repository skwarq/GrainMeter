import { useCallback, useEffect, useMemo, useState } from 'preact/hooks';
import * as api from './api.js';
import { translate, DEFAULT_LANG, LANGS } from './i18n/translate.js';
import { describeError } from './lib/errors.js';
import { I18nContext, errorText } from './components/i18n.js';
import { StatusContext, usePoll, useHashRoute } from './components/hooks.js';
import { Icon, Logo } from './components/ui.jsx';
import { UnlockDialog } from './components/UnlockDialog.jsx';
import { Overview } from './pages/Overview.jsx';
import { History } from './pages/History.jsx';
import { Sensors } from './pages/Sensors.jsx';
import { Config } from './pages/Config.jsx';
import { Upload } from './pages/Upload.jsx';
import { Update } from './pages/Update.jsx';
import { Diagnostics } from './pages/Diagnostics.jsx';
import { Simulation } from './pages/Simulation.jsx';
import { Service } from './pages/Service.jsx';

const STATUS_POLL_MS = 4000;
const LANG_KEY = 'gm.lang';

const PAGES = [
  { id: 'overview', icon: 'overview', comp: Overview },
  { id: 'history', icon: 'clock', comp: History },
  { id: 'sensors', icon: 'sensors', comp: Sensors },
  { id: 'config', icon: 'config', comp: Config },
  { id: 'upload', icon: 'upload', comp: Upload },
  { id: 'update', icon: 'update', comp: Update },
  { id: 'diag', icon: 'diag', comp: Diagnostics },
  { id: 'sim', icon: 'sim', comp: Simulation, platform: 'pc-sim' },
  { id: 'service', icon: 'service', comp: Service },
];

function initialLang() {
  try {
    const l = localStorage.getItem(LANG_KEY);
    if (LANGS.includes(l)) return l;
  } catch {
    /* ignore */
  }
  return DEFAULT_LANG;
}

export function App({ children }) {
  const [lang, setLangState] = useState(initialLang);
  const [unlock, setUnlock] = useState(null);
  const route = useHashRoute();
  const poll = usePoll(api.getStatus, STATUS_POLL_MS);

  const i18n = useMemo(
    () => ({
      lang,
      t: (k, p) => translate(lang, k, p),
      setLang: (l) => {
        setLangState(l);
        try {
          localStorage.setItem(LANG_KEY, l);
        } catch {
          /* ignore */
        }
      },
    }),
    [lang],
  );
  const { t } = i18n;

  useEffect(() => {
    document.documentElement.lang = lang;
  }, [lang]);

  useEffect(() => {
    api.setPasswordPrompt(
      (info) =>
        new Promise((resolve) => {
          setUnlock({ message: info.message, resolve });
        }),
    );
    return () => api.setPasswordPrompt(null);
  }, []);

  const finishUnlock = useCallback(
    (pw) => {
      if (unlock) unlock.resolve(pw);
      setUnlock(null);
    },
    [unlock],
  );

  const status = poll.data;
  const platform = status ? status.platform : null;
  const pages = PAGES.filter((p) => !p.platform || p.platform === platform);
  const page = pages.find((p) => p.id === route) || (route === 'sim' && !status ? null : pages[0]);
  const Page = page ? page.comp : null;
  const offline = !!poll.error;

  useEffect(() => {
    if (page) document.title = `${t('nav.' + page.id)} · ${status && status.name ? status.name : 'GrainMeter'}`;
  }, [page, status, t]);

  const statusCtx = useMemo(
    () => ({ status, error: poll.error, fetchedAt: poll.fetchedAt, refresh: poll.refresh, setStatus: poll.set }),
    [status, poll.error, poll.fetchedAt, poll.refresh, poll.set],
  );

  return (
    <I18nContext.Provider value={i18n}>
      <StatusContext.Provider value={statusCtx}>
        {children}
        <div class="app">
          <header class="topbar">
            <a class="brand" href="#/">
              <Logo />
              <span class="brand-text">
                <strong>{status && status.name ? status.name : 'GrainMeter'}</strong>
                <small>{t('app.subtitle')}</small>
              </span>
            </a>
            <div class="topbar-right">
              <span
                class={'conn ' + (offline ? 'conn-off' : status ? 'conn-on' : 'conn-wait')}
                title={offline ? errorText(t, describeError(poll.error)) : ''}
              >
                <span class="dot" />
                <span class="conn-text">
                  {offline ? t('conn.offline') : status ? t('conn.online') : t('conn.connecting')}
                </span>
              </span>
              <button
                type="button"
                class="lang"
                onClick={() => i18n.setLang(lang === 'pl' ? 'en' : 'pl')}
                aria-label={t('app.switchLang')}
              >
                <Icon name="globe" size={16} />
                {lang === 'pl' ? 'EN' : 'PL'}
              </button>
            </div>
          </header>
          <nav class="nav" aria-label={t('app.nav')}>
            {pages.map((p) => (
              <a
                key={p.id}
                href={'#/' + p.id}
                class={page && page.id === p.id ? 'active' : ''}
                aria-current={page && page.id === p.id ? 'page' : undefined}
              >
                <Icon name={p.icon} size={18} />
                <span>{t('nav.' + p.id)}</span>
              </a>
            ))}
          </nav>
          {offline && (
            <div class="banner" role="status">
              <Icon name="warn" size={18} />
              <span>{errorText(t, describeError(poll.error))}</span>
            </div>
          )}
          <main class="main">{Page && <Page key={page.id} />}</main>
          <footer class="foot">
            <span>GrainMeter {status ? status.firmware : ''}</span>
            <span>{platform === 'pc-sim' ? t('app.simulator') : platform === 'esp32c6' ? 'ESP32-C6' : ''}</span>
          </footer>
        </div>
        {unlock && <UnlockDialog request={unlock} onDone={finishUnlock} />}
      </StatusContext.Provider>
    </I18nContext.Provider>
  );
}
