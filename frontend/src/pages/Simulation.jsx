import { useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n } from '../components/i18n.js';
import { useAction, useStatus } from '../components/hooks.js';
import { Card, Button, Badge, Result, Loading, PageHead, Empty } from '../components/ui.jsx';
import { formatRom } from '../lib/format.js';
import { formatTemp, isTemp } from '../lib/temp.js';

function Toggle({ label, desc, on, onSet, busy, onText, offText }) {
  return (
    <div class="toggle-row">
      <div>
        <strong>{label}</strong>
        {desc && <p class="muted small">{desc}</p>}
      </div>
      <div class="seg" role="group" aria-label={label}>
        <button type="button" class={!on ? 'active' : ''} disabled={busy} onClick={() => onSet(false)}>
          {offText}
        </button>
        <button type="button" class={on ? 'active on' : ''} disabled={busy} onClick={() => onSet(true)}>
          {onText}
        </button>
      </div>
    </div>
  );
}

export function Simulation() {
  const { t } = useI18n();
  const { status: s, refresh } = useStatus();
  const act = useAction();
  const [sensor, setSensor] = useState('1');
  const [count, setCount] = useState('1');
  const [permanent, setPermanent] = useState(false);
  const [temp, setTemp] = useState('0');
  const [speed, setSpeed] = useState('');
  const [adv, setAdv] = useState('900');
  const [batt, setBatt] = useState('');

  if (!s) return <Loading text={t('common.loading')} />;
  if (s.platform !== 'pc-sim' || !s.sim) return <Empty>{t('sim.notAvailable')}</Empty>;
  const sim = s.sim;
  const devices = (sim.probe && sim.probe.devices) || [];
  const cur = devices.find((d) => String(d.index) === sensor);
  const unsupportedOn = devices.some((d) => d.rom && d.rom.startsWith('10'));

  const run = (args) =>
    act
      .run(() => api.simAction(args), { ok: (r) => r.message || t('common.done') })
      .then((r) => {
        if (r) refresh();
      });

  return (
    <div>
      <PageHead title={t('nav.sim')} sub={t('sim.sub')} />
      <Result result={act.result} />
      <div class="grid2">
        <Card title={t('sim.sensor')} icon="sensors">
          <label class="field">
            <span>{t('sim.pickSensor')}</span>
            <select value={sensor} onChange={(e) => setSensor(e.currentTarget.value)}>
              {devices.map((d) => (
                <option key={d.index} value={String(d.index)}>
                  {`#${d.index} · ${formatRom(d.rom)} · ${d.present ? (isTemp(d.temp_c) ? formatTemp(d.temp_c, 2) : '—') : t('sim.removed')}`}
                </option>
              ))}
            </select>
          </label>
          {cur && (
            <div class="chips">
              <Badge tone={cur.present ? 'ok' : 'danger'}>{cur.present ? t('sim.present') : t('sim.removed')}</Badge>
              {cur.crc_faults !== 0 && (
                <Badge tone="warn">
                  {t('sim.crcFaults')}: {cur.crc_faults === -1 ? '∞' : cur.crc_faults}
                </Badge>
              )}
              <Badge>{isTemp(cur.temp_c) ? formatTemp(cur.temp_c, 2) : '—'}</Badge>
            </div>
          )}
          <div class="sim-actions">
            <div class="inline-form">
              <label class="field field-inline">
                <span>{t('sim.crcCount')}</span>
                <input
                  type="number"
                  min="1"
                  value={count}
                  disabled={permanent}
                  onInput={(e) => setCount(e.currentTarget.value)}
                />
              </label>
              <label class="check">
                <input type="checkbox" checked={permanent} onChange={(e) => setPermanent(e.currentTarget.checked)} />
                <span>{t('sim.permanent')}</span>
              </label>
              <Button busy={act.busy} onClick={() => run({ action: 'crc', sensor, count: permanent ? -1 : count })}>
                {t('sim.injectCrc')}
              </Button>
            </div>
            <div class="inline-form">
              <label class="field field-inline">
                <span>{t('sim.tempValue')}</span>
                <input
                  type="number"
                  step="0.1"
                  min="-55"
                  max="125"
                  value={temp}
                  onInput={(e) => setTemp(e.currentTarget.value)}
                />
              </label>
              <Button busy={act.busy} onClick={() => run({ action: 'temp', sensor, value: temp })}>
                {t('sim.setTemp')}
              </Button>
            </div>
            <div class="inline-form">
              <Button variant="danger-ghost" busy={act.busy} onClick={() => run({ action: 'remove', sensor })}>
                {t('sim.remove')}
              </Button>
              <Button busy={act.busy} onClick={() => run({ action: 'restore', sensor })}>
                {t('sim.restore')}
              </Button>
            </div>
          </div>
        </Card>

        <Card title={t('sim.faults')} icon="warn">
          <Toggle
            label={t('sim.unsupported')}
            desc={t('sim.unsupportedDesc')}
            on={unsupportedOn}
            busy={act.busy}
            onText={t('sim.on')}
            offText={t('sim.off')}
            onSet={(v) => run({ action: 'unsupported', arg: v ? 'on' : 'off' })}
          />
          <Toggle
            label={t('sim.shorted')}
            desc={t('sim.shortDesc')}
            on={!!(sim.probe && sim.probe.shorted)}
            busy={act.busy}
            onText={t('sim.on')}
            offText={t('sim.off')}
            onSet={(v) => run({ action: 'short', arg: v ? 'on' : 'off' })}
          />
          <Toggle
            label={t('sim.network')}
            desc={t('sim.netDesc')}
            on={!!sim.network_up}
            busy={act.busy}
            onText={t('sim.up')}
            offText={t('sim.down')}
            onSet={(v) => run({ action: 'net', arg: v ? 'up' : 'down' })}
          />
          <Toggle
            label={t('sim.lostResponse')}
            desc={t('sim.lostDesc')}
            on={!!sim.lost_response}
            busy={act.busy}
            onText={t('sim.on')}
            offText={t('sim.off')}
            onSet={(v) => run({ action: 'lost_response', arg: v ? 'on' : 'off' })}
          />
          <Toggle
            label={t('sim.otaFail')}
            desc={t('sim.otaFailDesc')}
            on={!!(sim.ota_slots && sim.ota_slots.fail_next_boot)}
            busy={act.busy}
            onText={t('sim.on')}
            offText={t('sim.off')}
            onSet={(v) => run({ action: 'ota_fail_boot', arg: v ? 'on' : 'off' })}
          />
          <div class="inline-form">
            <label class="field field-inline">
              <span>{t('sim.battery')}</span>
              <input
                type="number"
                min="0"
                max="5000"
                placeholder={sim.battery_override_mv > 0 ? String(sim.battery_override_mv) : '3400'}
                value={batt}
                onInput={(e) => setBatt(e.currentTarget.value)}
              />
            </label>
            <Button busy={act.busy} disabled={batt === ''} onClick={() => run({ action: 'battery', value: batt })}>
              {t('sim.setBattery')}
            </Button>
          </div>
          <p class="muted small">{t('sim.batteryDesc')}</p>
        </Card>
      </div>

      <Card title={t('sim.time')} icon="clock">
        <p class="muted small">{t('sim.speedNow', { x: sim.speed })}</p>
        <div class="sim-actions">
          <div class="inline-form">
            <label class="field field-inline">
              <span>{t('sim.speed')}</span>
              <input
                type="number"
                min="1"
                placeholder={String(sim.speed)}
                value={speed}
                onInput={(e) => setSpeed(e.currentTarget.value)}
              />
            </label>
            <Button busy={act.busy} disabled={!speed} onClick={() => run({ action: 'speed', value: speed })}>
              {t('sim.setSpeed')}
            </Button>
          </div>
          <div class="inline-form">
            <label class="field field-inline">
              <span>{t('sim.advance')}</span>
              <select value={adv} onChange={(e) => setAdv(e.currentTarget.value)}>
                <option value="60">1 min</option>
                <option value="900">15 min</option>
                <option value="3600">1 h</option>
                <option value="21600">6 h</option>
                <option value="86400">24 h</option>
              </select>
            </label>
            <Button busy={act.busy} onClick={() => run({ action: 'advance', seconds: adv })}>
              {t('sim.advanceBtn')}
            </Button>
          </div>
          <div class="inline-form">
            <Button variant="danger-ghost" busy={act.busy} onClick={() => run({ action: 'powerloss' })}>
              {t('sim.powerloss')}
            </Button>
            <span class="muted small">{t('sim.powerlossDesc')}</span>
          </div>
        </div>
      </Card>
    </div>
  );
}
