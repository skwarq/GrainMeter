import { test } from 'node:test';
import assert from 'node:assert/strict';
import { isIpv4, powerTone, updateTone, validateNetworkAndPower } from '../src/lib/power.js';

test('isIpv4', () => {
  assert.equal(isIpv4('192.168.1.50'), true);
  assert.equal(isIpv4('0.0.0.0'), true);
  assert.equal(isIpv4('192.168.1.256'), false);
  assert.equal(isIpv4('10.0.0'), false);
  assert.equal(isIpv4('10.0.0.1.2'), false);
  assert.equal(isIpv4('10.0.0.-1'), false);
  assert.equal(isIpv4('a.b.c.d'), false);
  assert.equal(isIpv4(''), false);
  assert.equal(isIpv4(null), false);
});

test('tones', () => {
  assert.equal(powerTone('normal'), 'ok');
  assert.equal(powerTone('economy'), 'warn');
  assert.equal(powerTone('critical'), 'danger');
  assert.equal(powerTone(undefined), 'neutral');
  assert.equal(updateTone('failed'), 'danger');
  assert.equal(updateTone('up_to_date'), 'ok');
  assert.equal(updateTone('never_checked'), 'neutral');
});

const base = { static_ip: '', gateway: '', netmask: '', dns: '', batt_low_mv: '3450', batt_crit_mv: '3300' };

test('validateNetworkAndPower', () => {
  assert.equal(validateNetworkAndPower(base), '');
  assert.equal(validateNetworkAndPower({ ...base, static_ip: '10.0.0.5' }), 'config.errStaticNeedsGw');
  assert.equal(
    validateNetworkAndPower({ ...base, static_ip: '10.0.0.5', gateway: '10.0.0.1', netmask: '255.255.255.0' }),
    '',
  );
  assert.equal(validateNetworkAndPower({ ...base, dns: '1.1.1' }), 'config.errIp');
  assert.equal(validateNetworkAndPower({ ...base, batt_low_mv: '3300', batt_crit_mv: '3400' }), 'config.errBattOrder');
  assert.equal(validateNetworkAndPower({ ...base, batt_low_mv: '1000' }), 'config.errBattRange');
  assert.equal(validateNetworkAndPower({ ...base, batt_low_mv: '0', batt_crit_mv: '3300' }), '');
});
