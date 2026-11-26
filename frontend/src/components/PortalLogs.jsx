import { useEffect, useState } from 'preact/hooks';
import { getLogs, getConfig, saveConfig, remoteLogModel } from '../api.js';
import { PortalLogs as LogModel } from '../lib/logs.js';
import { Logs } from './LogView.jsx';
import { useI18n, errorText } from './i18n.js';
import { describeError } from '../lib/errors.js';

// Where logs go (portal only / UDP / MQTT) and their level, next to the live
// view; the same fields as in Config -> Logs.
function LogTarget({ c, setC }) {
  const { t } = useI18n();
  const [msg, setMsg] = useState('');
  if (!c) return null;
  const mode = c.log_udp ? c.log_dest || 'udp' : 'portal';
  const save = async (next) => {
    setMsg('');
    try {
      await saveConfig({ log_udp: next.log_udp ? 1 : 0, log_dest: next.log_dest || 'udp', log_level: next.log_level });
      setC(next);
      setMsg(t('config.saved'));
    } catch (e) {
      setMsg(errorText(t, describeError(e)));
    }
  };
  return (
    <div class="form-grid">
      <label class="field">
        <span>{t('config.logMode')}</span>
        <select
          value={mode}
          onChange={(e) => {
            const m = e.currentTarget.value;
            save({ ...c, log_udp: m !== 'portal', log_dest: m === 'portal' ? c.log_dest : m });
          }}
        >
          <option value="portal">{t('config.logModePortal')}</option>
          <option value="udp">UDP</option>
          <option value="mqtt">MQTT</option>
        </select>
      </label>
      <label class="field">
        <span>{t('config.logLevel')}</span>
        <select value={String(c.log_level)} onChange={(e) => save({ ...c, log_level: Number(e.currentTarget.value) })}>
          <option value="1">{t('config.logLevel1')}</option>
          <option value="2">{t('config.logLevel2')}</option>
          <option value="3">{t('config.logLevel3')}</option>
          <option value="4">{t('config.logLevel4')}</option>
        </select>
      </label>
      {msg && <p class="muted small field-wide">{msg}</p>}
    </div>
  );
}

export function PortalLogs() {
  const { t, lang } = useI18n();
  const [c, setC] = useState(null);
  const [model] = useState(() => new LogModel(getLogs));
  useEffect(() => {
    getConfig().then(setC, () => {});
  }, []);
  useEffect(() => () => model.setOpen(false), [model]);
  // In a remote session with logs sent over MQTT, show the pushed feed
  // instead of polling /api/logs through the tunnel (saves device work).
  const pushed = c && c.log_udp && c.log_dest === 'mqtt' ? remoteLogModel() : null;
  return (
    <>
      <LogTarget c={c} setC={setC} />
      {pushed ? (
        <Logs model={pushed} lang={lang} />
      ) : (
        <Logs model={model} lang={lang} preview describeError={(e) => errorText(t, describeError(e))} />
      )}
    </>
  );
}
