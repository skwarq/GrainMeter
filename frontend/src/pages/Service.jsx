import { useEffect, useRef, useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n } from '../components/i18n.js';
import { useAction, useStatus } from '../components/hooks.js';
import { Card, Button, Result, Loading, PageHead, KV } from '../components/ui.jsx';
import { formatDuration } from '../lib/format.js';

function Confirm({ text, onYes, onNo, busy }) {
  const { t } = useI18n();
  return (
    <div class="confirm" role="alertdialog">
      <p>{text}</p>
      <div class="row-actions">
        <Button onClick={onNo} disabled={busy}>
          {t('common.cancel')}
        </Button>
        <Button variant="danger" busy={busy} onClick={onYes}>
          {t('common.confirm')}
        </Button>
      </div>
    </div>
  );
}

export function Service() {
  const { t } = useI18n();
  const { status: s, refresh } = useStatus();
  const restartAct = useAction();
  const exitAct = useAction();
  const [ask, setAsk] = useState(null);
  const [, bump] = useState(0);
  const restartRefresh = useRef({ active: true, timer: 0 });
  useEffect(() => {
    const current = restartRefresh.current;
    current.active = true;
    return () => {
      current.active = false;
      clearTimeout(current.timer);
    };
  }, []);

  if (!s) return <Loading text={t('common.loading')} />;
  const esp = s.platform === 'esp32c6';

  const doRestart = () => {
    setAsk(null);
    restartAct
      .run(() => api.restart(), { ok: () => t('service.restarting') })
      .then((r) => {
        if (r && restartRefresh.current.active) restartRefresh.current.timer = setTimeout(refresh, 4000);
      });
  };
  const doExit = () => {
    setAsk(null);
    exitAct.run(() => api.exitService(), { ok: (r) => r.message || t('service.exited') });
  };

  return (
    <div>
      <PageHead title={t('nav.service')} sub={t('service.sub')} />
      <div class="grid2">
        <Card title={t('service.restart')} icon="refresh">
          <p class="muted">{t('service.restartDesc')}</p>
          {ask === 'restart' ? (
            <Confirm
              text={t('service.restartConfirm')}
              busy={restartAct.busy}
              onYes={doRestart}
              onNo={() => setAsk(null)}
            />
          ) : (
            <Button variant="primary" icon="refresh" busy={restartAct.busy} onClick={() => setAsk('restart')}>
              {t('service.restart')}
            </Button>
          )}
          <Result result={restartAct.result} />
        </Card>
        {esp && (
          <Card title={t('service.exit')} icon="service">
            <p class="muted">{t('service.exitDesc')}</p>
            {s.esp && (
              <KV
                items={[
                  ['AP', s.esp.ap_ssid],
                  [t('diag.apClients'), s.esp.ap_clients],
                  [t('diag.serviceTimeout'), formatDuration(s.esp.service_timeout_s)],
                ]}
              />
            )}
            {ask === 'exit' ? (
              <Confirm text={t('service.exitConfirm')} busy={exitAct.busy} onYes={doExit} onNo={() => setAsk(null)} />
            ) : (
              <Button variant="danger" icon="service" busy={exitAct.busy} onClick={() => setAsk('exit')}>
                {t('service.exit')}
              </Button>
            )}
            <Result result={exitAct.result} />
          </Card>
        )}
        <Card title={t('service.session')} icon="lock">
          <p class="muted">{api.hasPassword() ? t('service.unlocked') : t('service.locked')}</p>
          <Button
            disabled={!api.hasPassword()}
            onClick={() => {
              api.forgetPassword();
              bump((n) => n + 1);
            }}
          >
            {t('service.lock')}
          </Button>
        </Card>
      </div>
    </div>
  );
}
