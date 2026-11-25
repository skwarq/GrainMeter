import { useEffect, useRef, useState } from 'preact/hooks';
import * as api from '../api.js';
import { useI18n, errorText } from './i18n.js';
import { Button, Badge, Icon } from './ui.jsx';
import { describeError } from '../lib/errors.js';

const POLL_MS = 1500;
const JOIN_LIMIT_MS = 45000;

/** dBm -> 0..4 bars */
export function signalBars(rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

function joinMessage(t, j) {
  const m = j.message || '';
  if (/wrong Wi-Fi password/i.test(m)) return t('wifi.wrongPass', { ssid: j.ssid });
  if (/not found/i.test(m)) return t('wifi.notFound', { ssid: j.ssid });
  return m || t('wifi.joinFailed');
}

/**
 * Network list from a device scan + "connect and save" (like WiFiManager):
 * picking a network fills the SSID field, the join runs on the device while
 * this component polls the result.
 * @param {{form: object, onPick: (ssid:string, secure:boolean)=>void,
 *          joinForm: ()=>object|string, onJoined: ()=>void}} props
 * joinForm returns the request body or an error text.
 */
export function WifiPicker({ form, onPick, joinForm, onJoined }) {
  const { t } = useI18n();
  const [nets, setNets] = useState(null);
  const [scanning, setScanning] = useState(false);
  const [joining, setJoining] = useState(false);
  const [result, setResult] = useState(null);
  const alive = useRef(true);
  useEffect(() => () => (alive.current = false), []);

  const doScan = async () => {
    setScanning(true);
    setResult(null);
    try {
      const r = await api.wifiScan();
      if (alive.current) setNets(r.networks || []);
    } catch (e) {
      if (alive.current && e.kind !== 'aborted') setResult({ type: 'error', text: errorText(t, describeError(e)) });
    } finally {
      if (alive.current) setScanning(false);
    }
  };

  const doJoin = async () => {
    const body = joinForm();
    if (typeof body === 'string') {
      setResult({ type: 'error', text: body });
      return;
    }
    setJoining(true);
    setResult({ type: 'info', text: t('wifi.joinConnecting', { ssid: body.wifi_ssid }) });
    try {
      await api.wifiJoin(body);
    } catch (e) {
      if (alive.current) {
        setJoining(false);
        setResult(e.kind === 'aborted' ? null : { type: 'error', text: errorText(t, describeError(e)) });
      }
      return;
    }
    // The device may switch the SoftAP channel to the new network's channel:
    // the phone can drop off for a moment, so poll errors are not fatal.
    const deadline = Date.now() + JOIN_LIMIT_MS;
    while (alive.current && Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, POLL_MS));
      let j;
      try {
        j = await api.getWifiJoin();
      } catch {
        continue;
      }
      if (j.state === 'ok') {
        setJoining(false);
        setResult({ type: 'ok', text: t('wifi.joined', { ssid: j.ssid, ip: j.ip || '?', rssi: j.rssi }) });
        onJoined();
        return;
      }
      if (j.state === 'failed') {
        setJoining(false);
        setResult({ type: 'error', text: joinMessage(t, j) });
        return;
      }
    }
    if (alive.current) {
      setJoining(false);
      setResult({ type: 'error', text: t('wifi.noAnswer') });
    }
  };

  return (
    <div class="field-wide wifi-picker">
      <div class="row-actions">
        <Button icon="wifi" small busy={scanning} disabled={joining} onClick={doScan}>
          {nets ? t('wifi.rescan') : t('wifi.scan')}
        </Button>
        <Button variant="primary" small busy={joining} disabled={scanning || !form.wifi_ssid.trim()} onClick={doJoin}>
          {t('wifi.join')}
        </Button>
      </div>
      {nets && nets.length === 0 && <p class="muted small">{t('wifi.none')}</p>}
      {nets && nets.length > 0 && (
        <ul class="wifi-list" role="listbox" aria-label={t('wifi.list')}>
          {nets.map((n) => {
            const sel = n.ssid === form.wifi_ssid;
            const bars = signalBars(n.rssi);
            return (
              <li key={n.ssid}>
                <button
                  type="button"
                  role="option"
                  aria-selected={sel}
                  class={'wifi-item' + (sel ? ' is-selected' : '')}
                  onClick={() => onPick(n.ssid, n.secure)}
                >
                  <span class={'wifi-bars bars-' + bars} title={`${n.rssi} dBm`} aria-hidden="true">
                    <i />
                    <i />
                    <i />
                    <i />
                  </span>
                  <span class="wifi-ssid">{n.ssid}</span>
                  <span class="muted small">
                    {n.rssi} dBm · {t('wifi.channel', { ch: n.channel })}
                  </span>
                  {n.secure ? <Icon name="lock" size={14} /> : <Badge tone="warn">{t('wifi.open')}</Badge>}
                </button>
              </li>
            );
          })}
        </ul>
      )}
      <p class="muted small">{t('wifi.hint')}</p>
      {result && (
        <p class={'result result-' + result.type} role={result.type === 'error' ? 'alert' : 'status'}>
          {result.text}
        </p>
      )}
    </div>
  );
}
