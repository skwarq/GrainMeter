import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadRemote, saveRemote, REMOTE_KEY } from '../src/lib/remote-store.js';

function memory() {
  const m = new Map();
  return {
    getItem: (k) => (m.has(k) ? m.get(k) : null),
    setItem: (k, v) => m.set(k, String(v)),
    removeItem: (k) => m.delete(k),
    m,
  };
}

test('remote settings shared by dashboard and remote.html: secrets only on request', () => {
  const s = memory();
  const form = {
    url: 'wss://b:8084/mqtt',
    username: 'w',
    password: 'p1',
    id: 'silos1',
    servicePassword: 's1',
    talkbackId: '5',
  };
  saveRemote(form, false, s);
  let got = loadRemote(s);
  assert.deepEqual(got.form, { url: 'wss://b:8084/mqtt', username: 'w', id: 'silos1', talkbackId: '5' });
  assert.equal(got.remembered, false);
  saveRemote(form, true, s);
  got = loadRemote(s);
  assert.equal(got.form.password, 'p1');
  assert.equal(got.form.servicePassword, 's1');
  assert.equal(got.remembered, true);
  saveRemote({}, false, s);
  assert.equal(s.getItem(REMOTE_KEY), null);
  s.setItem(REMOTE_KEY, '{bad');
  assert.deepEqual(loadRemote(s), { form: {}, remembered: false });
});
