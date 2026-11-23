import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ApiError, describeError, isOffline } from '../src/lib/errors.js';
import { MESSAGES } from '../src/i18n/translate.js';

test('network failures map to readable keys', () => {
  assert.equal(describeError(new ApiError('network')).key, 'err.network');
  assert.equal(describeError(new ApiError('timeout')).key, 'err.timeout');
  assert.equal(describeError(new ApiError('parse', 200)).key, 'err.parse');
  assert.ok(isOffline(new ApiError('network')));
  assert.ok(isOffline(new ApiError('timeout')));
  assert.ok(!isOffline(new ApiError('http', 401)));
});

test('HTTP status codes keep the server text as detail', () => {
  const d = describeError(new ApiError('http', 400, 'upload_s must be >= measure_s'));
  assert.deepEqual(d, { key: 'err.400', params: { status: 400 }, detail: 'upload_s must be >= measure_s' });
  assert.equal(describeError(new ApiError('http', 401, 'x')).key, 'err.401');
  assert.equal(describeError(new ApiError('http', 409)).key, 'err.409');
  assert.equal(describeError(new ApiError('http', 418)).key, 'err.http');
});

test('OTA context gives specific messages for every documented code', () => {
  const codes = {
    401: 'err.401',
    409: 'err.409',
    411: 'err.411',
    413: 'err.ota413',
    415: 'err.ota415',
    422: 'err.ota422',
    500: 'err.ota500',
  };
  for (const [code, key] of Object.entries(codes)) {
    assert.equal(describeError(new ApiError('http', Number(code)), 'ota').key, key, code);
  }
  assert.equal(describeError(new ApiError('http', 415)).key, 'err.415');
});

test('every error key exists in both languages', () => {
  const keys = Object.keys(MESSAGES.pl).filter((k) => k.startsWith('err.'));
  for (const k of ['err.network', 'err.timeout', 'err.parse', 'err.aborted', 'err.unknown', 'err.http']) {
    assert.ok(keys.includes(k), k);
  }
  for (const k of keys) assert.ok(MESSAGES.en[k], k);
});

test('garbage input does not throw', () => {
  assert.equal(describeError(null).key, 'err.unknown');
  assert.equal(describeError(new Error('boom')).detail, 'boom');
});
