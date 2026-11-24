// Helpers for battery protection, network settings and remote update status.

/** Badge tone for a power mode reported by the device. */
export function powerTone(mode) {
  if (mode === 'critical') return 'danger';
  if (mode === 'economy') return 'warn';
  if (mode === 'normal') return 'ok';
  return 'neutral';
}

/** Badge tone for remote_update.result. */
export function updateTone(result) {
  switch (result) {
    case 'updated':
    case 'up_to_date':
      return 'ok';
    case 'failed':
      return 'danger';
    case 'skipped':
    case 'service_requested':
    case 'available':
      return 'warn';
    default:
      return 'neutral';
  }
}

/** True for a dotted IPv4 address "a.b.c.d" (0..255, no leading +/-). */
export function isIpv4(s) {
  if (typeof s !== 'string') return false;
  const parts = s.split('.');
  if (parts.length !== 4) return false;
  return parts.every((p) => /^\d{1,3}$/.test(p) && Number(p) <= 255);
}

/**
 * Client-side checks mirroring the device validation. Returns a message key
 * (config.err*) or '' when the values are acceptable.
 */
export function validateNetworkAndPower(f) {
  for (const k of ['static_ip', 'gateway', 'netmask', 'dns']) {
    const v = (f[k] || '').trim();
    if (v !== '' && !isIpv4(v)) return 'config.errIp';
  }
  if ((f.static_ip || '').trim() !== '' && (!(f.gateway || '').trim() || !(f.netmask || '').trim())) {
    return 'config.errStaticNeedsGw';
  }
  const low = Number(f.batt_low_mv);
  const crit = Number(f.batt_crit_mv);
  const okMv = (v) => Number.isInteger(v) && (v === 0 || (v >= 2500 && v <= 4500));
  if (!okMv(low) || !okMv(crit)) return 'config.errBattRange';
  if (low !== 0 && crit > low) return 'config.errBattOrder';
  return '';
}
