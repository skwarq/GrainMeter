import * as api from '../api.js';
import { useI18n } from '../components/i18n.js';
import { useAction, useStatus, useTick } from '../components/hooks.js';
import { Card, Button, Badge, KV, Meter, Loading, PageHead, Icon } from '../components/ui.jsx';
import { formatDuration, formatEpoch } from '../lib/format.js';

// Diagnostic channel in the last window (battery points + full row).
const DIAG_TONE = { sent: 'ok', failed: 'danger', not_due: 'neutral', not_configured: 'neutral' };

export function Upload() {
  const { t, lang } = useI18n();
  const { status: s, fetchedAt, refresh } = useStatus();
  const act = useAction();
  useTick(1000);
  if (!s) return <Loading text={t('common.loading')} />;

  const since = fetchedAt ? (Date.now() - fetchedAt) / 1000 : 0;
  const lu = s.last_upload;
  const b = s.buffer || {};
  const fill = b.capacity ? b.count / b.capacity : 0;
  const sch = s.schedule || {};
  const locale = lang === 'pl' ? 'pl-PL' : 'en-GB';
  const r = act.result && act.result.data;

  const send = () =>
    act
      .run(() => api.uploadNow(), { ok: () => null })
      .then((res) => {
        if (res) act.setResult({ type: res.ok ? 'ok' : 'error', text: res.message, data: res });
        refresh();
      });

  return (
    <div>
      <PageHead
        title={t('nav.upload')}
        sub={t('upload.sub')}
        actions={
          <Button variant="primary" icon="upload" busy={act.busy} onClick={send}>
            {t('upload.sendNow')}
          </Button>
        }
      />
      {!s.upload_configured && (
        <p class="notice notice-warn">
          <Icon name="warn" size={18} />
          <span>{t('overview.uploadNotConfigured')}</span>
          <a href="#/config">{t('nav.config')} →</a>
        </p>
      )}
      {act.result && (
        <div class={'outcome outcome-' + act.result.type} role="status">
          <Icon name={act.result.type === 'ok' ? 'ok' : 'warn'} size={22} />
          <div>
            <strong>{act.result.type === 'ok' ? t('upload.sentOk') : t('upload.sentFail')}</strong>
            <p>{act.result.text}</p>
            {r && (
              <p class="muted small">
                HTTP {r.http_status > 0 ? r.http_status : '—'} · {t('upload.confirmed')}: {r.samples_confirmed}
              </p>
            )}
          </div>
        </div>
      )}
      <div class="grid2">
        <Card title={t('overview.lastUpload')} icon="upload">
          {lu ? (
            <>
              <div class="tile-main">
                <Badge tone={lu.ok ? 'ok' : 'danger'}>{lu.ok ? t('common.ok') : t('common.failed')}</Badge>
                <span class="big">{t('common.ago', { age: formatDuration(lu.age_s + since) })}</span>
              </div>
              <p class="msg">{lu.message}</p>
              <KV
                items={[
                  [t('upload.when'), formatEpoch(lu.epoch, locale)],
                  [t('upload.http'), lu.http_status > 0 ? String(lu.http_status) : t('upload.noResponse')],
                  [t('upload.sent'), lu.samples_sent],
                  [t('upload.confirmed'), lu.samples_confirmed],
                  [
                    t('upload.diagChannel'),
                    <Badge key="d" tone={DIAG_TONE[lu.diag] || (lu.diag_ok ? 'ok' : 'neutral')}>
                      {lu.diag ? t('upload.diag.' + lu.diag) : lu.diag_ok ? t('common.ok') : t('common.failed')}
                    </Badge>,
                  ],
                ]}
              />
            </>
          ) : (
            <p class="muted">{t('upload.never')}</p>
          )}
        </Card>
        <Card title={t('upload.bufferStats')} icon="db">
          <div class="tile-main">
            <span class="big">
              {b.count} <small>/ {b.capacity}</small>
            </span>
            <span class="muted small">{t('upload.samplesWaiting')}</span>
          </div>
          <Meter
            value={fill}
            tone={fill > 0.9 ? 'danger' : fill > 0.7 ? 'warn' : 'accent'}
            label={t('overview.buffer')}
          />
          <KV
            items={[
              [t('upload.seqRange'), b.count > 0 ? `${b.oldest_seq} … ${b.next_seq - 1}` : '—'],
              [t('upload.nextSeq'), b.next_seq],
              [t('upload.droppedOverflow'), b.dropped_overflow],
              [t('upload.droppedNoTime'), b.dropped_no_time],
              [t('upload.okCount'), b.upload_ok],
              [t('upload.failCount'), b.upload_fail],
              [
                t('upload.interval'),
                formatDuration(sch.upload_s) + ' · ' + t('upload.perUpload', { n: sch.samples_per_upload }),
              ],
              [t('upload.due'), sch.upload_due ? t('common.yes') : t('common.no')],
            ]}
          />
        </Card>
      </div>
    </div>
  );
}
