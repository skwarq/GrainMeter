import { useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n, tOr, errorText } from '../components/i18n.js';
import { useAction, useLoad, useStatus } from '../components/hooks.js';
import { Card, Button, Badge, Result, Loading, PageHead, Empty, Icon } from '../components/ui.jsx';
import { PointList } from './Overview.jsx';
import { describeError } from '../lib/errors.js';
import { formatRom, formatDuration, romFamily } from '../lib/format.js';
import { formatTemp, isTemp } from '../lib/temp.js';
import { POINT_COUNT } from '../lib/silo.js';

function MapEditor({ rom, entry, usedBy, onSaved }) {
  const { t } = useI18n();
  const act = useAction();
  const [point, setPoint] = useState(entry ? String(entry.point) : '');
  const [height, setHeight] = useState(entry && entry.height_cm >= 0 ? String(entry.height_cm) : '');

  const save = () =>
    act
      .run(() => api.mapSensor(rom, Number(point), height === '' ? -1 : Number(height)), {
        ok: (r) => r.message,
      })
      .then((r) => r && onSaved());
  const remove = () => act.run(() => api.unmapSensor(rom), { ok: (r) => r.message }).then((r) => r && onSaved());
  const heightBad = height !== '' && !/^\d{1,5}$/.test(height);

  return (
    <div class="mapedit">
      <label class="field field-inline">
        <span>{t('sensors.point')}</span>
        <select value={point} onChange={(e) => setPoint(e.currentTarget.value)}>
          <option value="">—</option>
          {Array.from({ length: POINT_COUNT }, (_, i) => i + 1).map((p) => {
            const other = usedBy.get(p);
            return (
              <option key={p} value={String(p)}>
                {other && other !== rom ? t('sensors.pointTaken', { p }) : String(p)}
              </option>
            );
          })}
        </select>
      </label>
      <label class="field field-inline">
        <span>{t('sensors.heightCm')}</span>
        <input
          type="number"
          inputMode="numeric"
          min="0"
          max="32767"
          placeholder={t('sensors.heightUnknown')}
          value={height}
          onInput={(e) => setHeight(e.currentTarget.value)}
          class={heightBad ? 'invalid' : ''}
        />
      </label>
      <div class="mapedit-actions">
        <Button small variant="primary" busy={act.busy} disabled={!point || heightBad} onClick={save}>
          {t('common.save')}
        </Button>
        {entry && (
          <Button small variant="danger-ghost" disabled={act.busy} onClick={remove}>
            {t('sensors.unmap')}
          </Button>
        )}
      </div>
      <Result result={act.result} />
    </div>
  );
}

export function Sensors() {
  const { t } = useI18n();
  const { status, setStatus } = useStatus();
  const diag = useLoad(() => api.getDiag());
  const scanAct = useAction();
  const autoAct = useAction();
  const measAct = useAction();
  const [store, setStore] = useState(false);
  const [measured, setMeasured] = useState(false);

  const d = diag.data;
  const scan = d && d.scan;
  const map = (d && d.rom_map) || [];
  const mapByRom = new Map(map.map((e) => [e.rom, e]));
  const usedBy = new Map(map.map((e) => [e.point, e.rom]));
  const devices = scan && scan.devices ? scan.devices : [];
  const onBus = new Set(devices.map((x) => x.rom));
  const missingMapped = map.filter((e) => !onBus.has(e.rom));
  const live = new Map();
  if (status && status.measurement) {
    for (const p of status.measurement.points) if (p.rom) live.set(p.rom, p);
    for (const u of status.measurement.unmapped || []) live.set(u.rom, u);
  }

  const doScan = () => scanAct.run(() => api.scan(), { ok: () => t('sensors.scanDone') }).then((r) => r && diag.set(r));
  const doAuto = () => autoAct.run(() => api.automap(), { ok: (r) => r.message }).then((r) => r && diag.reload());
  const doMeasure = () =>
    measAct
      .run(() => api.measure(store), { ok: () => t(store ? 'sensors.measuredStored' : 'sensors.measured') })
      .then((r) => {
        if (r) {
          setStatus(r);
          setMeasured(true);
        }
      });

  return (
    <div>
      <PageHead
        title={t('nav.sensors')}
        sub={t('sensors.sub')}
        actions={
          <>
            <Button icon="search" variant="primary" busy={scanAct.busy} onClick={doScan}>
              {t('sensors.scan')}
            </Button>
            <Button icon="link" busy={autoAct.busy} onClick={doAuto}>
              {t('sensors.automap')}
            </Button>
          </>
        }
      />
      <Result result={scanAct.result} />
      <Result result={autoAct.result} />

      {diag.loading && !d && <Loading text={t('common.loading')} />}
      {diag.error && !d && <p class="result result-error">{errorText(t, describeError(diag.error))}</p>}

      {scan && (
        <div class="chips">
          <Badge tone={scan.presence ? 'ok' : 'danger'}>
            {scan.presence ? t('diag.presenceYes') : t('diag.presenceNo')}
          </Badge>
          {scan.bus_error && <Badge tone="danger">{t('diag.busError')}</Badge>}
          {scan.rom_crc_errors > 0 && <Badge tone="warn">{t('diag.romCrc', { n: scan.rom_crc_errors })}</Badge>}
          {scan.truncated && <Badge tone="warn">{t('sensors.truncated')}</Badge>}
          <Badge>{t('sensors.found', { n: devices.length })}</Badge>
          {typeof scan.age_s === 'number' && (
            <span class="muted small">{t('sensors.scanAge', { age: formatDuration(scan.age_s) })}</span>
          )}
        </div>
      )}

      {d && (
        <Card title={t('sensors.devices')} icon="sensors">
          {devices.length === 0 && missingMapped.length === 0 && <Empty>{t('sensors.none')}</Empty>}
          <div class="devlist">
            {devices.map((dev) => {
              const entry = mapByRom.get(dev.rom);
              const reading = live.get(dev.rom);
              return (
                <article key={dev.rom} class={'dev' + (dev.supported ? '' : ' dev-unsupported')}>
                  <div class="dev-head">
                    <code class="rom">{formatRom(dev.rom)}</code>
                    <span class="dev-fam">
                      <Badge tone="neutral" title={t('sensors.familyCode')}>
                        {dev.family || '0x' + romFamily(dev.rom)}
                      </Badge>{' '}
                      {dev.family_name || t('common.unknown')}
                    </span>
                    {dev.supported ? (
                      entry ? (
                        <Badge tone="ok">{t('sensors.mappedTo', { p: entry.point })}</Badge>
                      ) : (
                        <Badge tone="warn">{t('sensors.notMapped')}</Badge>
                      )
                    ) : (
                      <Badge tone="danger">{t('sensors.unsupported')}</Badge>
                    )}
                    {reading && (
                      <span class={'dev-read' + (reading.status === 'ok' ? '' : ' text-danger')}>
                        {reading.status === 'ok' && isTemp(reading.temp_c)
                          ? formatTemp(reading.temp_c, 2)
                          : tOr(t, 'status.', reading.status)}
                      </span>
                    )}
                  </div>
                  {dev.supported ? (
                    <MapEditor
                      key={dev.rom + ':' + (entry ? entry.point + '/' + entry.height_cm : '-')}
                      rom={dev.rom}
                      entry={entry}
                      usedBy={usedBy}
                      onSaved={diag.reload}
                    />
                  ) : (
                    <p class="muted small">{t('sensors.unsupportedHint')}</p>
                  )}
                </article>
              );
            })}
            {missingMapped.map((e) => (
              <article key={e.rom} class="dev dev-missing">
                <div class="dev-head">
                  <code class="rom">{formatRom(e.rom)}</code>
                  <Badge tone="danger">{t('sensors.notOnBus')}</Badge>
                  <Badge tone="ok">{t('sensors.mappedTo', { p: e.point })}</Badge>
                </div>
                <MapEditor
                  key={e.rom + ':' + e.point + '/' + e.height_cm}
                  rom={e.rom}
                  entry={e}
                  usedBy={usedBy}
                  onSaved={diag.reload}
                />
              </article>
            ))}
          </div>
        </Card>
      )}

      <Card
        title={t('sensors.measureTitle')}
        icon="clock"
        actions={
          <>
            <label class="check">
              <input type="checkbox" checked={store} onChange={(e) => setStore(e.currentTarget.checked)} />
              <span>{t('sensors.store')}</span>
            </label>
            <Button variant="primary" icon="sensors" busy={measAct.busy} onClick={doMeasure}>
              {t('sensors.measureNow')}
            </Button>
          </>
        }
      >
        <Result result={measAct.result} />
        {status && status.measurement ? (
          <>
            <p class="muted small">
              {measured
                ? t('sensors.resultFresh')
                : t('sensors.resultLast', { age: formatDuration(status.measurement.age_s) })}{' '}
              · {t('diag.conversion')}: {status.measurement.conversion_ms} ms
              {status.measurement.conversion_timeout ? ' (' + t('diag.timeout') + ')' : ''}
            </p>
            <PointList points={status.measurement.points} />
            {status.measurement.unmapped && status.measurement.unmapped.length > 0 && (
              <>
                <h3 class="h3">{t('sensors.unmappedReadings')}</h3>
                <ul class="pointlist">
                  {status.measurement.unmapped.map((u) => (
                    <li key={u.rom}>
                      <Icon name="sensors" size={14} />
                      <code class="rom small">{formatRom(u.rom)}</code>
                      <span class="pl-temp">
                        {u.status === 'ok' && isTemp(u.temp_c) ? formatTemp(u.temp_c, 2) : '—'}
                      </span>
                      <span class="pl-status">
                        {u.status !== 'ok' && <Badge tone="danger">{tOr(t, 'status.', u.status)}</Badge>}
                      </span>
                    </li>
                  ))}
                </ul>
              </>
            )}
          </>
        ) : (
          <Empty>{t('overview.noMeasurement')}</Empty>
        )}
      </Card>
    </div>
  );
}
