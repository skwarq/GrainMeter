import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { MESSAGES, translate } from '../src/i18n/translate.js';

test('pl and en have the same keys and no empty texts', () => {
  const pl = Object.keys(MESSAGES.pl).sort();
  const en = Object.keys(MESSAGES.en).sort();
  assert.deepEqual(en, pl);
  for (const l of ['pl', 'en']) for (const [k, v] of Object.entries(MESSAGES[l])) assert.ok(v.trim(), `${l}:${k}`);
});

test('placeholders match between languages', () => {
  const ph = (s) => (s.match(/\{\w+\}/g) || []).sort().join();
  for (const k of Object.keys(MESSAGES.pl)) assert.equal(ph(MESSAGES.en[k]), ph(MESSAGES.pl[k]), k);
});

test('translate fills params and falls back', () => {
  assert.equal(translate('en', 'common.ago', { age: '5 s' }), '5 s ago');
  assert.equal(translate('xx', 'nav.sensors'), 'Czujniki');
  assert.equal(translate('pl', 'no.such.key'), 'no.such.key');
});

test("every literal t('key') used in src exists", () => {
  const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../src');
  const files = readdirSync(root, { recursive: true }).filter((f) => /\.(js|jsx)$/.test(f));
  const missing = [];
  for (const f of files) {
    const src = readFileSync(path.join(root, f), 'utf8');
    for (const m of src.matchAll(/\bt\('([a-z]\w*(?:\.\w+)+)'/g)) {
      if (!(m[1] in MESSAGES.pl)) missing.push(`${f}: ${m[1]}`);
    }
  }
  assert.deepEqual(missing, []);
});
