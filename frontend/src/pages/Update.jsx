import { useEffect, useRef, useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n, errorText, tOr } from '../components/i18n.js';
import { useStatus } from '../components/hooks.js';
import { Card, Button, Badge, KV, Meter, PageHead, Icon } from '../components/ui.jsx';
import { describeError } from '../lib/errors.js';
import { formatBytes } from '../lib/format.js';
import { inspectImage } from '../lib/ota.js';
import { updateTone } from '../lib/power.js';
import { formatDuration } from '../lib/format.js';

const RESTART_WAIT_S = 6;
const RECONNECT_LIMIT_S = 180;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function Update() {
  const { t } = useI18n();
  const { status: s, refresh } = useStatus();
  const [file, setFile] = useState(null);
  const [check, setCheck] = useState(null);
  const [phase, setPhase] = useState('idle'); // idle|uploading|restarting|back|error
  const [progress, setProgress] = useState({ sent: 0, total: 0 });
  const [error, setError] = useState('');
  const [reply, setReply] = useState(null);
  const [countdown, setCountdown] = useState(0);
  const [running, setRunning] = useState(null);
  const [origin, setOrigin] = useState('file'); // which card started the update
  const [remote, setRemote] = useState('idle'); // idle|checking|installing
  const [remoteError, setRemoteError] = useState('');
  const fromRemote = useRef(false);
  const upload = useRef(null);
  const alive = useRef(true);
  const input = useRef(null);
  useEffect(
    () => () => {
      alive.current = false;
      if (upload.current) upload.current.abort();
    },
    [],
  );

  const pick = async (e) => {
    const f = e.currentTarget.files && e.currentTarget.files[0];
    setFile(f || null);
    setCheck(null);
    setError('');
    setPhase('idle');
    if (f) {
      try {
        const head = new Uint8Array(await f.slice(0, 128).arrayBuffer());
        setCheck(inspectImage(head));
      } catch {
        setCheck(null);
      }
    }
  };

  const waitForDevice = async () => {
    setPhase('restarting');
    for (let i = RESTART_WAIT_S; i > 0 && alive.current; i--) {
      setCountdown(i);
      await sleep(1000);
    }
    setCountdown(0);
    const start = Date.now();
    while (alive.current && Date.now() - start < RECONNECT_LIMIT_S * 1000) {
      try {
        const st = await api.getStatus();
        if (!alive.current) return null;
        setRunning(st);
        setPhase('back');
        refresh();
        return st;
      } catch {
        await sleep(2000);
      }
    }
    if (alive.current) {
      setPhase('error');
      const msg = t('update.noReturn', { s: RECONNECT_LIMIT_S });
      if (fromRemote.current) setRemoteError(msg);
      else setError(msg);
    }
    return null;
  };

  const remoteCheck = async () => {
    setRemoteError('');
    setRemote('checking');
    try {
      await api.remoteUpdateCheck();
    } catch (e) {
      if (alive.current) setRemoteError(errorText(t, describeError(e)));
    }
    if (!alive.current) return;
    setRemote('idle');
    refresh();
  };

  const remoteInstall = async () => {
    const v = s && s.remote_update ? s.remote_update.latest : '';
    if (!window.confirm(t('remote.confirmInstall', { v }))) return;
    setRemoteError('');
    setRemote('installing');
    const restarted = async (unsure) => {
      fromRemote.current = true;
      setOrigin('remote');
      setReply({ version: v });
      const st = await waitForDevice(); // shows the running version
      // After a lost connection we only guessed that it restarted: if the old
      // version still runs, the installation did not happen - say why.
      if (unsure && st && st.firmware !== v) {
        setPhase('idle');
        const ru = st.remote_update;
        setRemoteError(t('remote.notInstalled', { msg: (ru && ru.message) || '' }));
      }
    };
    try {
      const r = await api.remoteUpdateInstall(v);
      if (!alive.current) return;
      setRemote('idle');
      if (r && r.restarting) {
        await restarted(false);
        return;
      }
      setRemoteError(r && r.message ? r.message : t('remote.failed'));
    } catch (e) {
      if (!alive.current) return;
      setRemote('idle');
      // The phone may lose the device's Wi-Fi while it downloads (the access
      // point follows the router's channel) or the reply may come too late:
      // the device can still be installing and restarting.
      if (e && (e.kind === 'network' || e.kind === 'timeout')) {
        await restarted(true);
        return;
      }
      setRemoteError(errorText(t, describeError(e)));
    }
    refresh();
  };

  const start = async () => {
    if (!file) return;
    fromRemote.current = false;
    setOrigin('file');
    setError('');
    setReply(null);
    setProgress({ sent: 0, total: file.size });
    setPhase('uploading');
    const job = api.uploadFirmware(file, (sent, total) => setProgress({ sent, total }));
    upload.current = job;
    try {
      const r = await job.promise;
      upload.current = null;
      if (!alive.current) return;
      setReply(r);
      setProgress((p) => ({ ...p, sent: p.total }));
      await waitForDevice();
    } catch (e) {
      upload.current = null;
      if (!alive.current) return;
      if (e && e.kind === 'aborted') {
        setPhase('idle');
        setError(t('update.cancelled'));
        return;
      }
      setPhase('error');
      setError(errorText(t, describeError(e, 'ota')));
    }
  };

  const pct = progress.total ? Math.round((progress.sent / progress.total) * 100) : 0;
  const busy = phase === 'uploading' || phase === 'restarting' || remote !== 'idle';
  const ota = s && s.ota;

  const outcome = (
    <>
      {phase === 'restarting' && (
        <div class="outcome outcome-ok">
          <span class="spinner" />
          <div>
            <strong>{t('update.accepted', { v: reply && reply.version ? reply.version : '?' })}</strong>
            <p>{countdown > 0 ? t('update.restartIn', { s: countdown }) : t('update.waiting')}</p>
          </div>
        </div>
      )}

      {phase === 'back' && running && (
        <div class="outcome outcome-ok">
          <Icon name="ok" size={22} />
          <div>
            <strong>{t('update.back')}</strong>
            <p>{t('update.runningVersion', { v: running.firmware })}</p>
            {reply && reply.version && reply.version !== running.firmware && (
              <p class="text-warn small">{t('update.versionDiffers', { v: reply.version })}</p>
            )}
          </div>
        </div>
      )}
    </>
  );
  const ru = s && s.remote_update;
  const remoteBusy = remote !== 'idle' || busy;
  const canInstall = ru && ru.result === 'available' && ru.latest;

  return (
    <div>
      <PageHead title={t('nav.update')} sub={t('update.sub')} />
      <div class="grid2">
        <Card title={t('update.upload')} icon="update">
          <label class={'dropzone' + (busy ? ' disabled' : '')}>
            <input ref={input} type="file" onChange={pick} disabled={busy} />
            <Icon name="download" size={28} />
            <span>{file ? file.name : t('update.choose')}</span>
            <small class="muted">{file ? formatBytes(file.size) : t('update.hint')}</small>
          </label>
          {check && (
            <p class={'precheck ' + (check.ok ? 'text-ok' : 'text-warn')}>
              <Icon name={check.ok ? 'ok' : 'warn'} size={16} />
              <span>
                {check.ok
                  ? t('update.checkOk', { v: check.version || '?' })
                  : t('update.check.' + check.reason, { p: check.project || '' })}
              </span>
            </p>
          )}

          {phase === 'uploading' && (
            <div class="progress">
              <div class="progress-row">
                <span>{t('update.sending')}</span>
                <b>{pct} %</b>
              </div>
              <Meter value={pct / 100} label={t('update.sending')} />
              <small class="muted">
                {formatBytes(progress.sent)} / {formatBytes(progress.total)}
              </small>
            </div>
          )}

          {origin === 'file' && outcome}

          {error && (
            <p class="result result-error" role="alert">
              <Icon name="warn" size={16} />
              <span>{error}</span>
            </p>
          )}

          <div class="row-actions">
            {phase === 'uploading' ? (
              <Button variant="danger-ghost" onClick={() => upload.current && upload.current.abort()}>
                {t('common.cancel')}
              </Button>
            ) : (
              <Button variant="primary" icon="update" disabled={!file || busy} onClick={start}>
                {t('update.start')}
              </Button>
            )}
          </div>
          <p class="muted small">{t('update.note')}</p>
        </Card>

        {s && s.remote_update && (
          <Card title={t('remote.title')} icon="refresh">
            <KV
              items={[
                [t('remote.running'), s.remote_update.running],
                [
                  t('remote.result'),
                  <Badge key="r" tone={updateTone(s.remote_update.result)}>
                    {tOr(t, 'remote.', s.remote_update.result)}
                  </Badge>,
                ],
                [t('remote.auto'), s.remote_update.enabled ? t('remote.autoOn') : t('remote.autoOff')],
                s.remote_update.latest && [t('remote.latest'), s.remote_update.latest],
                s.remote_update.message && [t('common.message'), s.remote_update.message],
                s.remote_update.last_check_age_s !== null && [
                  t('remote.lastCheck'),
                  t('common.ago', { age: formatDuration(s.remote_update.last_check_age_s) }),
                ],
                s.remote_update.rejected && [
                  t('remote.rejected'),
                  <Badge key="rj" tone="danger">
                    {s.remote_update.rejected}
                  </Badge>,
                ],
              ]}
            />
            {remote !== 'idle' && (
              <div class="outcome outcome-ok">
                <span class="spinner" />
                <div>
                  <strong>{t(remote === 'checking' ? 'remote.checking' : 'remote.installing')}</strong>
                  {remote === 'installing' && <p>{t('remote.installingHint')}</p>}
                </div>
              </div>
            )}
            {origin === 'remote' && outcome}
            {remoteError && (
              <p class="result result-error" role="alert">
                <Icon name="warn" size={16} />
                <span>{remoteError}</span>
              </p>
            )}
            {canInstall && remote === 'idle' && phase !== 'restarting' && (
              <p class="precheck text-ok">
                <Icon name="download" size={16} />
                <span>{t('remote.newer', { v: ru.latest, r: ru.running })}</span>
              </p>
            )}
            <div class="row-actions">
              <Button
                icon="refresh"
                disabled={!ru.url_set || remoteBusy}
                busy={remote === 'checking'}
                onClick={remoteCheck}
              >
                {t('remote.check')}
              </Button>
              {canInstall && (
                <Button
                  variant="primary"
                  icon="download"
                  disabled={remoteBusy}
                  busy={remote === 'installing'}
                  onClick={remoteInstall}
                >
                  {t('remote.install', { v: ru.latest })}
                </Button>
              )}
            </div>
            {!ru.url_set && <p class="muted small">{t('remote.noUrl')}</p>}
            <p class="muted small">{t('remote.note')}</p>
          </Card>
        )}

        <Card title={t('update.state')} icon="chip">
          <KV
            items={[
              [t('common.firmware'), s ? s.firmware : null],
              s && s.esp && [t('update.partition'), `${s.esp.running_partition} → ${s.esp.next_partition}`],
              s && s.esp && [t('update.appState'), s.esp.app_state],
              s && s.esp && [t('update.slotSize'), formatBytes(s.esp.slot_size)],
              s && s.sim && s.sim.ota_slots && [t('update.partition'), s.sim.ota_slots.running],
              ota && [
                t('update.otaState'),
                <Badge key="st" tone={ota.state === 'failed' ? 'danger' : ota.state === 'done' ? 'ok' : 'neutral'}>
                  {tOr(t, 'ota.', ota.state)}
                </Badge>,
              ],
              ota &&
                ota.state !== 'idle' && [
                  t('update.received'),
                  `${formatBytes(ota.received)}${ota.expected ? ' / ' + formatBytes(ota.expected) : ''}`,
                ],
              ota && ota.file && [t('update.file'), ota.file],
              ota && ota.version && [t('update.imgVersion'), ota.version],
              ota && ota.project && [t('update.project'), ota.project],
              ota && ota.idf && ['ESP-IDF', ota.idf],
              ota &&
                ota.error && [
                  t('update.lastError'),
                  <span key="e" class="text-danger">
                    {ota.error}
                  </span>,
                ],
            ]}
          />
        </Card>
      </div>
    </div>
  );
}
