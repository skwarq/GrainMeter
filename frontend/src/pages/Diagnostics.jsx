import { useRef, useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n, tOr, errorText } from '../components/i18n.js';
import { useAction, useLoad } from '../components/hooks.js';
import { Card, Button, Badge, KV, Loading, PageHead, Empty, Icon, Result } from '../components/ui.jsx';
import { describeError } from '../lib/errors.js';
import { formatDuration, formatRom, formatBytes, formatEpoch } from '../lib/format.js';
import { formatTemp, pointState, tempColor, isTemp } from '../lib/temp.js';
import { powerTone } from '../lib/power.js';
import { PortalLogs } from '../components/PortalLogs.jsx';

function yesNo(t, v, goodWhen = true) {
  const good = !!v === goodWhen;
  return <Badge tone={good ? 'ok' : 'danger'}>{v ? t('common.yes') : t('common.no')}</Badge>;
}

function Counter({ label, value, tone }) {
  return (
    <div class={'counter' + (tone ? ' counter-' + tone : '')}>
      <b>{value === undefined || value === null ? '—' : String(value)}</b>
      <small>{label}</small>
    </div>
  );
}

function copyText(text) {
  if (navigator.clipboard && window.isSecureContext) return navigator.clipboard.writeText(text);
  // captive portal runs over plain http: use the legacy path
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.select();
  let ok;
  try {
    ok = document.execCommand('copy');
  } catch {
    ok = false;
  }
  document.body.removeChild(ta);
  return ok ? Promise.resolve() : Promise.reject(new Error('copy failed'));
}

function RawJson({ data }) {
  const { t } = useI18n();
  const [open, setOpen] = useState(false);
  const [copied, setCopied] = useState('');
  const pre = useRef(null);
  const text = JSON.stringify(data, null, 2);
  const copy = () =>
    copyText(text).then(
      () => setCopied(t('diag.copied')),
      () => {
        setCopied(t('diag.copyFailed'));
        if (pre.current) {
          const r = document.createRange();
          r.selectNodeContents(pre.current);
          const sel = window.getSelection();
          sel.removeAllRanges();
          sel.addRange(r);
        }
      },
    );
  const download = () => {
    const blob = new Blob([text + '\n'], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    a.href = url;
    a.download = `grainmeter-diag-${stamp}.json`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  return (
    <Card
      title={t('diag.raw')}
      icon="db"
      actions={
        <>
          <Button small icon="copy" onClick={copy}>
            {t('diag.copy')}
          </Button>
          <Button small icon="download" onClick={download}>
            {t('diag.download')}
          </Button>
          <Button small onClick={() => setOpen(!open)}>
            {open ? t('diag.hide') : t('diag.show')}
          </Button>
        </>
      }
    >
      {copied && <p class="muted small">{copied}</p>}
      {open ? (
        <pre ref={pre} class="json">
          {text}
        </pre>
      ) : (
        <p class="muted small">{t('diag.rawHint', { size: formatBytes(text.length) })}</p>
      )}
    </Card>
  );
}

export function Diagnostics() {
  const { t, lang } = useI18n();
  const diag = useLoad(() => api.getDiag());
  const d = diag.data;
  if (!d && diag.error) return <p class="result result-error">{errorText(t, describeError(diag.error))}</p>;
  if (!d) return <Loading text={t('common.loading')} />;

  const scan = d.scan || {};
  const m = d.measurement;
  const b = d.buffer || {};
  const ota = d.ota || {};
  const errors = (d.errors || []).slice().reverse();
  const locale = lang === 'pl' ? 'pl-PL' : 'en-GB';
  const presence = m ? m.presence : scan.presence;
  const busShort = !!scan.bus_error || !!(m && m.points.some((p) => p.status === 'bus_error'));
  const found = (scan.devices || []).length;
  const health = busShort ? 'short' : !presence ? 'nopresence' : found === 0 ? 'nodevices' : 'ok';
  const healthText = {
    short: t('diag.busError'),
    nopresence: t('diag.presenceNo'),
    nodevices: t('diag.noDevices'),
    ok: t('diag.presenceYes'),
  }[health];

  return (
    <div>
      <PageHead
        title={t('nav.diag')}
        sub={t('diag.sub')}
        actions={
          <Button icon="refresh" busy={diag.loading} onClick={diag.reload}>
            {t('common.refresh')}
          </Button>
        }
      />
      {diag.error && <p class="result result-error">{errorText(t, describeError(diag.error))}</p>}

      <div class="grid2">
        <Card title={t('diag.bus')} icon="link" tone={health === 'ok' ? undefined : 'danger'}>
          <div class="bus-health">
            <div class={'bus-light ' + (health === 'ok' ? 'ok' : 'bad')}>
              <Icon name={health === 'ok' ? 'ok' : 'warn'} size={22} />
            </div>
            <div>
              <strong>{healthText}</strong>
              <p class="muted small">{t('sensors.found', { n: (scan.devices || []).length })}</p>
            </div>
          </div>
          <KV
            items={[
              [t('diag.presence'), yesNo(t, presence)],
              [t('diag.busErrorLabel'), yesNo(t, busShort, false)],
              [
                t('diag.romCrcLabel'),
                <Badge key="c" tone={scan.rom_crc_errors ? 'warn' : 'ok'}>
                  {scan.rom_crc_errors ?? '—'}
                </Badge>,
              ],
              [t('diag.truncated'), yesNo(t, scan.truncated, false)],
              [t('diag.conversion'), m ? `${m.conversion_ms} ms` : '—'],
              [t('diag.conversionTimeout'), m ? yesNo(t, m.conversion_timeout, false) : '—'],
              [t('diag.scanAge'), typeof scan.age_s === 'number' ? formatDuration(scan.age_s) : '—'],
              [t('diag.measAge'), m ? formatDuration(m.age_s) : '—'],
            ]}
          />
        </Card>

        <Card title={t('diag.points')} icon="sensors">
          {m ? (
            <div class="pgrid">
              {m.points.map((p) => {
                const st = pointState(p);
                return (
                  <div key={p.point} class={'pcell pcell-' + st.state} title={p.rom || ''}>
                    <span class="pcell-n">{p.point}</span>
                    <span class="pcell-swatch" style={st.valid ? { background: tempColor(st.temp) } : undefined} />
                    <b>{st.valid ? formatTemp(st.temp) : '—'}</b>
                    <small>{tOr(t, 'status.', p.status)}</small>
                    <code>{p.rom ? p.rom.slice(-6) : '—'}</code>
                  </div>
                );
              })}
            </div>
          ) : (
            <Empty>{t('overview.noMeasurement')}</Empty>
          )}
          {m && m.unmapped && m.unmapped.length > 0 && (
            <p class="muted small">
              {t('overview.unmappedSensors', { n: m.unmapped.length })}:{' '}
              {m.unmapped.map((u) => `${formatRom(u.rom)} (${tOr(t, 'status.', u.status)})`).join(', ')}
            </p>
          )}
        </Card>
      </div>

      <Card title={t('diag.counters')} icon="db">
        <div class="counters">
          <Counter label={t('upload.okCount')} value={b.upload_ok} tone="ok" />
          <Counter label={t('upload.failCount')} value={b.upload_fail} tone={b.upload_fail ? 'danger' : undefined} />
          <Counter
            label={t('upload.droppedOverflow')}
            value={b.dropped_overflow}
            tone={b.dropped_overflow ? 'warn' : undefined}
          />
          <Counter
            label={t('upload.droppedNoTime')}
            value={b.dropped_no_time}
            tone={b.dropped_no_time ? 'warn' : undefined}
          />
          <Counter label={t('diag.wakeCount')} value={b.wake_count} />
          <Counter label={t('overview.buffer')} value={`${b.count}/${b.capacity}`} />
          <Counter
            label={t('diag.powerLoss')}
            value={b.recovered_after_power_loss ? t('common.yes') : t('common.no')}
            tone={b.recovered_after_power_loss ? 'warn' : undefined}
          />
        </div>
      </Card>

      <div class="grid2">
        <Card title={t('diag.errors')} icon="warn">
          {errors.length === 0 ? (
            <Empty>{t('diag.noErrors')}</Empty>
          ) : (
            <ol class="timeline">
              {errors.map((e, i) => (
                <li key={i}>
                  <span class="tl-age">{t('common.ago', { age: formatDuration(e.age_s) })}</span>
                  <span class="tl-text">{e.text}</span>
                </li>
              ))}
            </ol>
          )}
        </Card>

        <Card title={t('diag.system')} icon="chip">
          <KV
            items={[
              [t('common.firmware'), d.firmware],
              [t('diag.platform'), d.platform],
              [t('diag.uptime'), formatDuration(d.mono_s)],
              [t('diag.wake'), d.wake_reason],
              [t('overview.reset'), d.reset_reason],
              [
                t('overview.time'),
                d.time_valid ? (
                  formatEpoch(d.epoch, locale) || t('common.yes')
                ) : (
                  <Badge key="tv" tone="warn">
                    {t('overview.timeInvalid')}
                  </Badge>
                ),
              ],
              [t('overview.battery'), typeof d.battery_mv === 'number' ? d.battery_mv + ' mV' : '—'],
              d.power && [
                t('power.mode'),
                <span key="pm">
                  <Badge tone={powerTone(d.power.mode)}>{tOr(t, 'power.', d.power.mode)}</Badge>{' '}
                  <span class="muted small">
                    {t('power.thresholds', { low: d.power.low_mv || '—', crit: d.power.crit_mv || '—' })}
                  </span>
                </span>,
              ],
              d.remote_update && [
                t('remote.title'),
                d.remote_update.enabled
                  ? `${tOr(t, 'remote.', d.remote_update.result)}${
                      d.remote_update.message ? ' — ' + d.remote_update.message : ''
                    }`
                  : t('remote.disabled'),
              ],
              d.remote_update && d.remote_update.rejected && [t('remote.rejected'), d.remote_update.rejected],
              [
                'Wi-Fi',
                d.wifi ? `${tOr(t, 'wifi.', d.wifi.state)}${d.wifi.rssi < 0 ? ' · ' + d.wifi.rssi + ' dBm' : ''}` : '—',
              ],
              [t('update.otaState'), `${tOr(t, 'ota.', ota.state)}${ota.error ? ' — ' + ota.error : ''}`],
              ota.version && [t('update.imgVersion'), `${ota.version} (${ota.project || '?'})`],
            ]}
          />
        </Card>
      </div>

      {d.esp && (
        <Card title={t('diag.esp')} icon="chip">
          <KV
            items={[
              [t('diag.serviceMode'), yesNo(t, d.esp.service_mode)],
              ['AP SSID', d.esp.ap_ssid],
              ['AP IP', d.esp.ap_ip],
              [t('diag.apClients'), d.esp.ap_clients],
              [t('diag.serviceTimeout'), formatDuration(d.esp.service_timeout_s)],
              [t('update.partition'), `${d.esp.running_partition} → ${d.esp.next_partition}`],
              [t('update.slotSize'), formatBytes(d.esp.slot_size)],
              [t('update.appState'), d.esp.app_state],
              [t('diag.freeHeap'), formatBytes(d.esp.free_heap)],
              ['1-Wire GPIO', d.esp.onewire_gpio],
              [t('diag.wakeGpio'), d.esp.service_wake_gpio < 0 ? '—' : d.esp.service_wake_gpio],
              d.esp.rejected_firmware && [t('remote.rejected'), d.esp.rejected_firmware],
            ]}
          />
        </Card>
      )}

      {d.esp && d.esp.uptime_s !== undefined && <EspDetails esp={d.esp} onCleared={() => diag.reload()} />}

      {d.sim && (
        <Card title={t('diag.sim')} icon="sim">
          <KV
            items={[
              [t('sim.speed'), '×' + d.sim.speed],
              [t('sim.network'), yesNo(t, d.sim.network_up)],
              [t('sim.lostResponse'), yesNo(t, d.sim.lost_response, false)],
              [t('diag.httpPosts'), d.sim.http_posts],
              [t('diag.mockEntries'), `${d.sim.mock_entries_main} / ${d.sim.mock_entries_diag}`],
              [t('diag.configFile'), d.sim.config_file],
              [t('diag.timeSyncs'), d.sim.time_syncs],
              [t('diag.httpSessions'), d.sim.http_sessions],
              [t('diag.serviceRequests'), d.sim.service_requests],
              d.sim.battery_override_mv > 0 && [t('diag.batteryOverride'), d.sim.battery_override_mv + ' mV'],
              d.sim.ota_slots && d.sim.ota_slots.rejected && [t('remote.rejected'), d.sim.ota_slots.rejected],
              d.sim.ota_slots && [
                t('update.partition'),
                `${d.sim.ota_slots.running}${d.sim.ota_slots.pending_new_image ? ' (+ ' + t('diag.pendingImage') + ')' : ''}`,
              ],
              d.sim.probe && [t('sim.shorted'), yesNo(t, d.sim.probe.shorted, false)],
            ]}
          />
          {d.sim.probe && d.sim.probe.devices && (
            <div class="table-wrap">
              <table class="table">
                <thead>
                  <tr>
                    <th>#</th>
                    <th>ROM</th>
                    <th>{t('sim.present')}</th>
                    <th>{t('sim.crcFaults')}</th>
                    <th>{t('sim.temp')}</th>
                  </tr>
                </thead>
                <tbody>
                  {d.sim.probe.devices.map((x) => (
                    <tr key={x.index}>
                      <td>{x.index}</td>
                      <td>
                        <code>{formatRom(x.rom)}</code>
                      </td>
                      <td>{yesNo(t, x.present)}</td>
                      <td>{x.crc_faults === -1 ? '∞' : x.crc_faults}</td>
                      <td>{isTemp(x.temp_c) ? formatTemp(x.temp_c, 2) : '—'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </Card>
      )}

      <RawJson data={d} />
      <PortalLogs />
    </div>
  );
}

/** WIFI_RS style device diagnostics (ESP32 only): system, memory, station
 *  link, UDP log counters, FreeRTOS tasks and the last crash (core dump). */
function EspDetails({ esp, onCleared }) {
  const { t } = useI18n();
  const act = useAction();
  const st = esp.station || {};
  const crash = esp.crash || { present: false };
  const clear = () =>
    act.run(() => api.clearCrash(), { ok: (r) => r.message || t('diag.crashCleared') }).then((r) => r && onCleared());
  const tasks = (esp.tasks || []).slice().sort((a, b) => a.name.localeCompare(b.name));
  return (
    <>
      <Card title={t('diag.espSystem')} icon="chip">
        <KV
          items={[
            [t('diag.sinceBoot'), formatDuration(esp.uptime_s)],
            [t('diag.bootCount'), esp.boot_count],
            [t('diag.build'), esp.build],
            ['ESP-IDF', esp.idf],
            [
              t('diag.chipTemp'),
              esp.chip_temp_c === null || esp.chip_temp_c === undefined ? '—' : esp.chip_temp_c.toFixed(1) + ' °C',
            ],
            esp.heap && [t('diag.freeHeap'), formatBytes(esp.heap.free)],
            esp.heap && [t('diag.minFreeHeap'), formatBytes(esp.heap.min_free)],
            esp.heap && [t('diag.largestBlock'), formatBytes(esp.heap.largest_block)],
            [
              t('diag.station'),
              st.connected ? `${st.ssid} · ${st.ip} · ${t('wifi.channel', { ch: st.channel })} · ${st.rssi} dBm` : '—',
            ],
            st.connected && ['BSSID', st.bssid],
            esp.udp_log && [
              t('diag.udpLog'),
              t('diag.udpLogCounts', { sent: esp.udp_log.sent, dropped: esp.udp_log.dropped }),
            ],
            esp.logs && [t('config.logMode'), esp.logs.log_dest.toUpperCase()],
            esp.logs && [
              t('diag.mqttLog'),
              t('diag.udpLogCounts', { sent: esp.logs.mqtt_sent, dropped: esp.logs.mqtt_dropped }),
            ],
          ]}
        />
        <p class="muted small">{t('diag.chipTempHint')}</p>
      </Card>

      <Card title={t('diag.crash')} icon="warn" tone={crash.present ? 'danger' : undefined}>
        {crash.present ? (
          <>
            <KV
              items={[
                [t('diag.crashTask'), crash.task],
                ['PC', crash.pc],
                ['mcause', crash.mcause],
                crash.reason && [t('diag.crashReason'), crash.reason],
                ['ELF SHA-256', crash.elf_sha256],
              ]}
            />
            <Result result={act.result} />
            <div class="row-actions">
              <Button small busy={act.busy} onClick={clear}>
                {t('diag.crashClear')}
              </Button>
            </div>
          </>
        ) : (
          <p class="muted">{t('diag.noCrash')}</p>
        )}
      </Card>

      {tasks.length > 0 && (
        <Card title={t('diag.tasks')} icon="db">
          <div class="table-wrap">
            <table class="table">
              <thead>
                <tr>
                  <th>{t('diag.taskName')}</th>
                  <th class="num">{t('diag.stackFree')}</th>
                  <th class="num">{t('diag.prio')}</th>
                </tr>
              </thead>
              <tbody>
                {tasks.map((k) => (
                  <tr key={k.name}>
                    <td>{k.name}</td>
                    <td class={'num' + (k.stack_free < 512 ? ' text-danger' : '')}>{k.stack_free} B</td>
                    <td class="num">{k.prio}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Card>
      )}
    </>
  );
}
