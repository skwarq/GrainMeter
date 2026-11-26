import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { uploadFirmware } from '../src/lib/ota-upload.js';
import { ApiError } from '../src/lib/errors.js';
import * as facade from '../src/api.js';

function setup(size = 3001, chunk) {
  const bytes = Uint8Array.from({ length: size }, (_, i) => i % 256);
  if (size) bytes[0] = 0xe9;
  const file = new File([bytes], 'firmware.bin');
  const sha256 = createHash('sha256').update(bytes).digest('hex');
  const calls = [];
  let received = 0;
  let writes = 0;
  const api = {
    async otaBegin(form, signal) {
      calls.push('begin');
      assert.deepEqual(form, { size, sha256, name: file.name });
      assert.equal(signal?.aborted || false, false);
      return { ok: true, next: 0, ...(chunk === undefined ? {} : { chunk }) };
    },
    async otaChunk({ off, d }) {
      calls.push(off);
      assert.match(d, /^[A-Za-z0-9_-]+$/);
      const decoded = Buffer.from(d, 'base64url');
      assert.ok(decoded.length <= (chunk || 1440));
      assert.deepEqual(decoded, Buffer.from(bytes.subarray(off, off + decoded.length)));
      if (off === received) {
        received += decoded.length;
        writes++;
      } else assert.ok(off + decoded.length <= received);
      return { ok: true, next: received };
    },
    async getOta() {
      calls.push('status');
      return { received, expected: size, chunk: chunk || 1440 };
    },
    async otaFinish(form) {
      calls.push('finish');
      assert.equal(received, size);
      assert.deepEqual(form, { sha256 });
      return { ok: true, version: '1.2.3', bytes: received, restarting: true };
    },
    async otaAbort() {
      calls.push('abort');
      return { ok: true };
    },
  };
  return { api, file, bytes, calls, writes: () => writes };
}

test('full upload, SHA-256, default chunks, acknowledged progress and ETA', async () => {
  const { file, api, calls } = setup();
  const progress = [];
  const result = await uploadFirmware(file, api, { onProgress: (p) => progress.push(p) });
  assert.equal(result.restarting, true);
  assert.deepEqual(calls, ['begin', 0, 1440, 2880, 'finish']);
  assert.deepEqual(
    progress.map((p) => p.sent),
    [0, 1440, 2880, 3001, 3001],
  );
  assert.equal(progress[0].eta, null);
  assert.ok(progress[1].eta >= 0);
  assert.equal(progress.at(-1).percent, 100);
  assert.equal(progress.at(-1).eta, 0);
  assert.equal(progress.at(-1).stage, 'finishing');
});

test('honours begin chunk size, including one- and two-byte base64url tails', async () => {
  for (const chunk of [1, 2, 4]) {
    const { file, api } = setup(7, chunk);
    await uploadFirmware(file, api);
  }
});

test('lost chunk response retries identical bytes; duplicate is acknowledged without another write', async () => {
  const fixture = setup();
  const send = fixture.api.otaChunk;
  let lost = true;
  fixture.api.otaChunk = async (form) => {
    const response = await send(form);
    if (lost) {
      lost = false;
      throw new ApiError('timeout');
    }
    return response;
  };
  await uploadFirmware(fixture.file, fixture.api);
  assert.deepEqual(fixture.calls, ['begin', 0, 0, 1440, 2880, 'finish']);
  assert.equal(fixture.writes(), 3);
});

test('409 offset resumes from signed next, including a backwards correction', async () => {
  const { file, api, calls } = setup();
  const send = api.otaChunk;
  let corrected = false;
  api.otaChunk = async (form) => {
    if (form.off === 1440 && !corrected) {
      corrected = true;
      const e = new ApiError('http', 409, 'offset');
      e.next = 0;
      throw e;
    }
    return send(form);
  };
  await uploadFirmware(file, api);
  assert.deepEqual(calls, ['begin', 0, 0, 1440, 2880, 'finish']);
});

test('broken connection resumes from GET /api/ota received, not the unconfirmed local offset', async () => {
  const { file, api, calls } = setup();
  const send = api.otaChunk;
  let broken = true;
  api.otaChunk = async (form) => {
    const result = await send(form);
    if (broken) {
      broken = false;
      throw new ApiError('network');
    }
    return result;
  };
  await uploadFirmware(file, api);
  assert.deepEqual(calls, ['begin', 0, 'status', 1440, 2880, 'finish']);
});

test('at most three retries per chunk; fatal image errors are not retried', async () => {
  for (const kind of ['timeout', 'network', 'http']) {
    const { file, api, calls } = setup();
    let attempts = 0;
    api.otaChunk = async () => {
      attempts++;
      throw new ApiError(kind, kind === 'http' ? 422 : 0, 'image');
    };
    await assert.rejects(uploadFirmware(file, api), (e) => e.kind === kind);
    assert.equal(attempts, kind === 'http' ? 1 : 4);
    assert.deepEqual(calls, kind === 'network' ? ['begin', 'status', 'status', 'status'] : ['begin']);
  }
});

test('bad magic and empty files fail before begin', async () => {
  const { api, calls } = setup();
  for (const data of [[], [0, 1, 2]]) {
    await assert.rejects(uploadFirmware(new File([new Uint8Array(data)], 'bad.bin'), api), (e) => e.status === 415);
  }
  assert.deepEqual(calls, []);
});

test('cancellation aborts the device without the cancelled signal and never finishes', async () => {
  const { file, api, calls } = setup();
  const controller = new AbortController();
  const send = api.otaChunk;
  api.otaChunk = async (form) => {
    await send(form);
    controller.abort();
    throw new ApiError('aborted');
  };
  api.otaAbort = async (...args) => {
    assert.deepEqual(args, []);
    calls.push('abort');
  };
  await assert.rejects(uploadFirmware(file, api, { signal: controller.signal }), (e) => e.kind === 'aborted');
  assert.deepEqual(calls, ['begin', 0, 'abort']);
});

test("pre-cancelled upload does not start or abort somebody else's session", async () => {
  const { file, api, calls } = setup();
  await assert.rejects(uploadFirmware(file, api, { signal: AbortSignal.abort() }), (e) => e.kind === 'aborted');
  assert.deepEqual(calls, []);
});

test('abort failure is surfaced rather than reported as successful cancellation', async () => {
  const { file, api } = setup();
  const controller = new AbortController();
  api.otaChunk = async () => {
    controller.abort();
    throw new ApiError('aborted');
  };
  api.otaAbort = async () => {
    throw new ApiError('network');
  };
  await assert.rejects(uploadFirmware(file, api, { signal: controller.signal }), (e) => e.kind === 'network');
});

test('422 sha256 at finish is surfaced without a retry or success', async () => {
  const { file, api, calls } = setup();
  api.otaFinish = async () => {
    calls.push('finish');
    throw new ApiError('http', 422, 'sha256');
  };
  await assert.rejects(uploadFirmware(file, api), (e) => e.status === 422 && e.serverMessage === 'sha256');
  assert.deepEqual(calls, ['begin', 0, 1440, 2880, 'finish']);
});

test('invalid offsets, stalled responses and mismatched recovered sessions cannot finish', async () => {
  for (const next of [-1, 3002, undefined, 0]) {
    const { file, api } = setup();
    api.otaChunk = async () => ({ next });
    await assert.rejects(uploadFirmware(file, api), (e) => e.kind === 'parse');
  }
  const { file, api } = setup();
  api.otaChunk = async () => {
    throw new ApiError('network');
  };
  api.getOta = async () => ({ expected: 1, received: 0 });
  await assert.rejects(uploadFirmware(file, api), (e) => e.status === 409);
});

test('OTA facade forwards forms, signals and 60 s begin/finish limits to current transport', async () => {
  const calls = [];
  facade.setRemoteTransport({
    request: async (...args) => {
      calls.push(args);
      return { ok: true };
    },
  });
  try {
    const signal = new AbortController().signal;
    await facade.otaBegin({ size: 1, sha256: 'hash', name: 'a.bin' }, signal);
    await facade.otaChunk({ off: 0, d: '6Q' }, signal);
    await facade.otaFinish({ sha256: 'hash' }, signal);
    await facade.getOta(signal);
    await facade.otaAbort();
    assert.deepEqual(
      calls.map(([method, path]) => [method, path]),
      [
        ['POST', '/api/ota/begin'],
        ['POST', '/api/ota/chunk'],
        ['POST', '/api/ota/finish'],
        ['GET', '/api/ota'],
        ['POST', '/api/ota/abort'],
      ],
    );
    assert.equal(calls[0][2].timeout, 60000);
    assert.equal(calls[2][2].timeout, 60000);
    assert.equal(calls[0][2].signal, signal);
    assert.deepEqual(calls[1][2].form, { off: 0, d: '6Q' });
    assert.equal(calls[4][2].signal, undefined);
  } finally {
    facade.setRemoteTransport(null);
  }
});
