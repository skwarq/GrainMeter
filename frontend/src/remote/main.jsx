import { render } from 'preact';
import { useState } from 'preact/hooks';
import { App } from '../app.jsx';
import { setRemoteTransport } from '../api.js';
import { MqttTransport } from '../transport/mqtt.js';
import { translate } from '../i18n/translate.js';
import { SessionBar } from './SessionBar.jsx';
import { sessionSnapshot } from './session.js';
import '../styles.css';
import '../components/history.css';
import { REMOTE_FIELDS as FIELDS, loadRemote, saveRemote } from '../lib/remote-store.js';
import './remote.css';

const DASH_KEY = 'gm.dashboard.v1';
// A link from scripts/remote_link.py carries the fields in the URL fragment
// (#cfg=<base64 JSON>); the fragment never reaches the server and is cleared
// from the address bar right after reading.
function fromLink() {
  const m = /^#cfg=([A-Za-z0-9+/=_-]+)$/.exec(location.hash);
  if (!m) return null;
  history.replaceState(null, '', location.pathname + location.search);
  try {
    const b = atob(m[1].replace(/-/g, '+').replace(/_/g, '/'));
    const v = JSON.parse(new TextDecoder().decode(Uint8Array.from(b, (c) => c.charCodeAt(0))));
    // ThingSpeak channel IDs in the link prefill the dashboard (same origin).
    if (/^\d{1,9}$/.test(v.channelId || '')) {
      try {
        const d = JSON.parse(localStorage.getItem(DASH_KEY)) || {};
        localStorage.setItem(
          DASH_KEY,
          JSON.stringify({
            ...d,
            channelId: d.channelId || v.channelId,
            diagChannelId: d.diagChannelId || (/^\d{1,9}$/.test(v.diagChannelId || '') ? v.diagChannelId : ''),
          }),
        );
      } catch {
        /* no storage */
      }
    }
    return Object.fromEntries(FIELDS.filter((k) => typeof v[k] === 'string').map((k) => [k, v[k]]));
  } catch {
    return null;
  }
}
function saved() {
  // Shared with the dashboard settings (Settings -> Remote access).
  const stored = loadRemote();
  const link = fromLink();
  // Remembering passwords is opt-in: only secrets already stored in this
  // browser tick "remember", never a #cfg link.
  return { form: link ? { ...stored.form, ...link } : stored.form, remembered: stored.remembered };
}
function Remote() {
  const [initial] = useState(saved);
  const [form, setForm] = useState(initial.form);
  const [remember, setRemember] = useState(initial.remembered);
  const [lang, setLang] = useState('pl');
  const [transport, setTransport] = useState(null);
  const [status, setStatus] = useState({ state: 'offline' });
  const [session, setSession] = useState(null);
  const [ended, setEnded] = useState(false);
  const [error, setError] = useState('');
  const [waiting, setWaiting] = useState(false);
  const [busy, setBusy] = useState(false);
  const [ready, setReady] = useState(false);
  const [expanded, setExpanded] = useState(true);
  const t = (key) => translate(lang, 'connection.' + key);
  const persist = (remember, value) => {
    try {
      saveRemote(value, remember);
    } catch {
      setError(t('storageError'));
    }
  };
  const connect = async () => {
    setBusy(true);
    setError('');
    persist(remember, form);
    transport?.close();
    setReady(false);
    setRemoteTransport(null);
    setTransport(null);
    setStatus({ state: 'offline' });
    setSession(null);
    setEnded(false);
    let next;
    try {
      if (!/^wss:\/\//.test(form.url || '') || !/^[a-z0-9-]{1,24}$/.test(form.id || '')) throw new Error(t('invalid'));
      next = new MqttTransport(form);
      next.onstatus = (s) => {
        setStatus(s);
        if (s.state === 'online' && s.verified) {
          setWaiting(false);
          setEnded(false);
          setSession(sessionSnapshot(s));
        } else setSession(null);
      };
      next.onresponse = (data, path) => {
        const info = data.remote || data;
        if (info && typeof info === 'object' && 'remaining_s' in info) setSession(sessionSnapshot(info));
        if (path === '/api/service/exit') setEnded(true);
      };
      setTransport(next);
      await next.connect();
      setRemoteTransport(next);
      setReady(true);
      setExpanded(false);
    } catch (e) {
      next?.close();
      setTransport(null);
      const why = { auth: 'authError', denied: 'deniedError', network: 'networkError', timeout: 'networkError' }[
        e?.message
      ];
      setError((why ? t(why) : t('connectError')) + (e?.topic ? ' [' + e.topic + ']' : ''));
    } finally {
      setBusy(false);
    }
  };
  const wake = async () => {
    setBusy(true);
    setError('');
    try {
      if (!/^[1-9][0-9]*$/.test(form.talkbackId || '') || !form.talkbackKey) throw new Error();
      const minutes = Number(form.minutes || 10);
      if (!Number.isInteger(minutes) || minutes < 1 || minutes > 60) throw new Error();
      const response = await fetch('https://api.thingspeak.com/talkbacks/' + form.talkbackId + '/commands.json', {
        method: 'POST',
        body: new URLSearchParams({
          api_key: form.talkbackKey,
          command_string: 'REMOTE ' + minutes + ' ' + (Math.floor(Date.now() / 1000) + 7200),
        }),
        signal: AbortSignal.timeout(15000),
      });
      if (!response.ok) throw new Error();
      setWaiting(true);
      if (!ready || (transport?.status.state === 'offline' && transport?.client.ws?.readyState !== 1)) await connect();
    } catch {
      setError(t('wakeError'));
    } finally {
      setBusy(false);
    }
  };
  const stateText =
    (status.state === 'deriving' ? t('deriving') : status.state === 'online' ? t('online') : t('offline')) +
    (status.state === 'online' && status.verified && status.until > 0
      ? ' · ' + t('until') + ' ' + new Date(status.until * 1000).toLocaleString(lang)
      : '');
  // One header in both states: device, state, language and the fold toggle
  // stay in place; the session bar sits right under it; the connection form
  // unfolds below (folded by default once connected).
  const open = !ready || expanded;
  return (
    <>
      <section class="card remote-connection">
        <div class="remote-head">
          <h1>{ready && form.id ? form.id : t('title')}</h1>
          <span role="status">{stateText}</span>
          {status.state !== 'online' && <a href="./">{t('dashboard')} →</a>}
          <button type="button" onClick={() => setLang(lang === 'pl' ? 'en' : 'pl')}>
            {lang === 'pl' ? 'EN' : 'PL'}
          </button>
          {ready && (
            <button
              type="button"
              class="remote-fold"
              onClick={() => setExpanded(!expanded)}
              aria-expanded={open ? 'true' : 'false'}
              title={t('title')}
            >
              {open ? '▴' : '▾'}
            </button>
          )}
        </div>
        {ready && (
          <SessionBar
            online={status.state === 'online' && status.verified}
            session={session}
            ended={ended}
            lang={lang}
          />
        )}
        {error && <p role="alert">{error}</p>}
        {open && (
          <>
            <form
              onSubmit={(e) => {
                e.preventDefault();
                connect();
              }}
            >
              {FIELDS.map((key) => (
                <label key={key}>
                  {t(key)}
                  <input
                    type={
                      ['password', 'servicePassword', 'talkbackKey'].includes(key)
                        ? 'password'
                        : key === 'minutes'
                          ? 'number'
                          : 'text'
                    }
                    min={key === 'minutes' ? 1 : undefined}
                    max={key === 'minutes' ? 60 : undefined}
                    required={!['talkbackId', 'talkbackKey', 'minutes'].includes(key)}
                    autoComplete="off"
                    value={form[key] || (key === 'minutes' ? 10 : '')}
                    onInput={(e) => {
                      const value = { ...form, [key]: e.currentTarget.value };
                      setForm(value);
                      persist(remember, value);
                    }}
                  />
                </label>
              ))}
              <label>
                <input
                  type="checkbox"
                  checked={remember}
                  onChange={(e) => {
                    setRemember(e.currentTarget.checked);
                    persist(e.currentTarget.checked, form);
                  }}
                />
                {t('remember')}
              </label>
              <button disabled={busy} type="submit">
                {t('connect')}
              </button>
              <button disabled={busy} type="button" onClick={wake}>
                {t('wake')}
              </button>
              {transport && (
                <button
                  type="button"
                  onClick={() => {
                    transport.close();
                    setTransport(null);
                    setReady(false);
                    setRemoteTransport(null);
                    setStatus({ state: 'offline' });
                  }}
                >
                  {t('disconnect')}
                </button>
              )}
            </form>
            <p>{t('wakeHint')}</p>
            {waiting && <p role="status">{t('waiting')}</p>}
            <p role="alert">{t('wifiWarning')}</p>
          </>
        )}
      </section>
      {ready && <App />}
    </>
  );
}
render(<Remote />, document.getElementById('app'));
