import { test } from 'node:test';
import assert from 'node:assert/strict';
import { build } from 'esbuild';
import { RemoteLogs, LOG_LIMIT, plainLog, logLevel, logText } from '../src/remote/logs.js';
import { MqttTransport, hmac, macText, deriveKeys, cryptBody, statusMacText } from '../src/transport/mqtt.js';
import { PortalLogs } from '../src/lib/logs.js';
import { getLogs, setRemoteTransport } from '../src/api.js';

const bundle = await build({
  stdin: {
    contents: "export { Logs } from './src/remote/Logs.jsx';",
    resolveDir: new URL('..', import.meta.url).pathname,
  },
  bundle: true,
  write: false,
  format: 'esm',
  platform: 'node',
  jsx: 'automatic',
  jsxImportSource: 'preact',
  plugins: [
    {
      name: 'log-view-fixtures',
      setup(b) {
        b.onResolve({ filter: /^preact\/hooks$/ }, () => ({ path: 'hooks', namespace: 'fixtures' }));
        b.onLoad({ filter: /^hooks$/, namespace: 'fixtures' }, () => ({
          contents: 'export const useState = (v) => [v, () => {}]; export const useEffect = () => {};',
        }));
      },
    },
  ],
});
const { Logs } = await import(
  'data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64')
);

function nodes(tree) {
  if (!tree || typeof tree !== 'object') return [];
  if (Array.isArray(tree)) return tree.flatMap(nodes);
  return [tree, ...nodes(tree.props?.children)];
}

test('collapsible bilingual panel renders hostile HTML as a text child, with pause and clear actions', () => {
  const model = new RemoteLogs();
  model.append('<script>alert(1)</script>');
  model.setState('denied');
  for (const lang of ['pl', 'en']) {
    const tree = Logs({ model, lang });
    assert.equal(tree.type, 'details');
    const elements = nodes(tree);
    const pre = elements.find((node) => node.type === 'pre');
    assert.equal(pre.props.children, '<script>alert(1)</script>\n');
    assert.ok(!elements.some((node) => node.props?.dangerouslySetInnerHTML));
    assert.ok(elements.some((node) => node.props?.role === 'alert' && node.props.children === logText(lang, 'denied')));
    const buttons = elements.filter((node) => node.type === 'button');
    assert.equal(buttons.at(-1).props.children, logText(lang, 'download'));
  }
  const buttons = nodes(Logs({ model, lang: 'en' })).filter((node) => node.type === 'button');
  buttons[0].props.onClick();
  assert.equal(model.paused, true);
  buttons[1].props.onClick();
  assert.equal(model.text(), '');
});

test('portal panel opens polling, displays bilingual loss, config hint and literal text', () => {
  for (const lang of ['pl', 'en']) {
    const model = new PortalLogs(async () => ({ next: 0, lost: 0, lines: [] }));
    model.append('<script>plain text</script>');
    model.lost = 12;
    model.error = new Error('offline');
    const tree = Logs({ model, lang, preview: true });
    const elements = nodes(tree);
    assert.equal(elements.find((n) => n.type === 'pre').props.children, '<script>plain text</script>\n');
    assert.ok(elements.some((n) => n.type === 'a' && n.props.href === '#/config'));
    assert.ok(elements.some((n) => n.props?.role === 'status' && n.props.children.includes(12)));
    assert.ok(elements.some((n) => n.props?.role === 'alert' && n.props.children.includes('offline')));
    tree.props.onToggle({ currentTarget: { open: true } });
    assert.equal(model.open, true);
    const buttons = elements.filter((n) => n.type === 'button');
    buttons[0].props.onClick();
    assert.equal(model.paused, true);
    assert.equal(model.controller.signal.aborted, true);
    assert.ok(
      nodes(Logs({ model, lang, preview: true })).some((n) => n.props?.children === logText(lang, 'previewPaused')),
    );
    model.setOpen(false);
  }
});

test('portal log facade sends cursor in path and abort signal through selected tunnel transport', async () => {
  const signal = new AbortController().signal;
  setRemoteTransport({
    async request(method, path, options) {
      assert.equal(method, 'GET');
      assert.equal(path, '/api/logs/42');
      assert.equal(options.signal, signal);
      return { next: 45, lost: 0, lines: ['I (1) app: ok'] };
    },
  });
  try {
    assert.equal((await getLogs(42, signal)).next, 45);
  } finally {
    setRemoteTransport(null);
  }
});

test('ESP-IDF filters recognize ANSI and device prefixes; unknown lines remain visible', () => {
  const logs = new RemoteLogs();
  logs.append(
    '\x1b[31m[gm-silo] E (123) wifi: failed\x1b[0m\r\n' +
      'gm-silo: W (124) wifi: weak\nI (125) app: online\nD (126) app: debug\n' +
      '<img src=x onerror=alert(1)>\n',
  );
  assert.deepEqual(
    logs.lines.map((line) => line.level),
    ['E', 'W', 'I', 'D', null],
  );
  assert.equal(logs.visible(['E', 'W']).length, 3);
  assert.equal(logs.visible([])[0].text, '<img src=x onerror=alert(1)>');
  assert.equal(logLevel('INFO: not an ESP-IDF severity'), null);
  assert.equal(plainLog('\x1b]0;malicious title\x07I (1) tag: ok\x00'), 'I (1) tag: ok');
  assert.equal(logs.text(['D']), 'D (126) app: debug\n<img src=x onerror=alert(1)>\n');
});

test('system HH:MM:SS.sss timestamps filter all ESP-IDF levels with device prefixes and ANSI colors', () => {
  const logs = new RemoteLogs();
  logs.append(
    '\x1b[31m[gm-silo] E (20:29:11.830) wifi: failed\x1b[0m\n' +
      'gm-silo: W (00:00:00.001) wifi: weak\nI (23:59:59.999) app: online\n' +
      'D (01:02:03.004) app: debug\n... 7 lines dropped\n',
  );
  assert.deepEqual(
    logs.lines.map((line) => line.level),
    ['E', 'W', 'I', 'D', null],
  );
  assert.equal(logs.text(['W']), 'gm-silo: W (00:00:00.001) wifi: weak\n... 7 lines dropped\n');
  assert.equal(logs.visible(['E', 'I']).length, 3);
  assert.equal(logLevel('I (20:29:11.8300) malformed timestamp'), null);
});

test('buffer is bounded across grouped payloads and oversized batches, pause drops new lines and clear empties', () => {
  const logs = new RemoteLogs();
  let updates = 0;
  const unsubscribe = logs.subscribe(() => updates++);
  for (let i = 0; i < 600; i++) logs.append('I (' + i + ') app: line\n');
  assert.equal(logs.lines.length, LOG_LIMIT);
  assert.equal(logs.lines[0].text, 'I (100) app: line');
  logs.pause(true);
  logs.append('E (600) app: discarded');
  assert.equal(logs.lines.at(-1).text, 'I (599) app: line');
  logs.clear();
  assert.equal(logs.text(), '');
  logs.pause(false);
  logs.append(Array.from({ length: 1000 }, (_, i) => String(i)).join('\n') + '\n');
  assert.equal(logs.lines.length, LOG_LIMIT);
  assert.equal(logs.lines[0].text, '500');
  assert.equal(logs.lines.at(-1).text, '999');
  assert.ok(updates > 600);
  unsubscribe();
  const before = updates;
  logs.clear();
  assert.equal(updates, before);
});

test('portal polling follows next, bounds and filters lines, pauses without resetting cursor', async () => {
  const requests = [];
  let scheduled;
  const timers = {
    setTimeout(fn, delay) {
      assert.equal(delay, 2000);
      scheduled = fn;
      return 1;
    },
    clearTimeout() {
      scheduled = null;
    },
  };
  const model = new PortalLogs(async (since) => {
    requests.push(since);
    return {
      next: since + 600,
      lost: 7,
      lines: Array.from({ length: 600 }, (_, i) => `${i % 2 ? 'E' : 'D'} (${i}) app: line`),
    };
  }, timers);
  const flush = () => new Promise((resolve) => setImmediate(resolve));
  model.setOpen(true);
  await flush();
  assert.deepEqual(requests, [0]);
  assert.equal(model.lines.length, 500);
  assert.equal(model.visible(['E']).length, 250);
  assert.equal(model.lost, 7);
  scheduled();
  await flush();
  assert.deepEqual(requests, [0, 600]);
  model.pause(true);
  assert.equal(scheduled, null);
  model.clear();
  assert.equal(model.lost, 0);
  assert.equal(model.text(), '');
  model.pause(false);
  await flush();
  assert.deepEqual(requests, [0, 600, 1200]);
  model.setOpen(false);
  assert.equal(scheduled, null);
});

test('portal ignores aborted late pages and reports malformed responses and request failures', async () => {
  let resolve;
  let signal;
  let scheduled;
  const model = new PortalLogs(
    (since, s) => {
      signal = s;
      return new Promise((r) => {
        resolve = r;
      });
    },
    {
      setTimeout(fn) {
        scheduled = fn;
      },
      clearTimeout() {
        scheduled = null;
      },
    },
  );
  const flush = () => new Promise((r) => setImmediate(r));
  model.setOpen(true);
  model.pause(true);
  assert.equal(signal.aborted, true);
  resolve({ next: 50, lost: 20, lines: ['late'] });
  await flush();
  assert.equal(model.next, 0);
  assert.equal(model.lost, 0);
  assert.equal(model.text(), '');
  model.read = async () => ({ next: -1, lost: 0, lines: [] });
  model.pause(false);
  await flush();
  assert.match(model.error.message, /Invalid/);
  model.read = async () => {
    throw new Error('offline');
  };
  scheduled();
  await flush();
  assert.equal(model.error.message, 'offline');
  model.read = async () => ({ next: 1, lost: 0, lines: ['<img src=x>'] });
  scheduled();
  await flush();
  assert.equal(model.error, null);
  assert.equal(model.text(), '<img src=x>\n');
  model.setOpen(false);
});

test('denied optional log subscription and arbitrary text do not affect signed tunnel requests', async () => {
  const subscriptions = [];
  let transport;
  const options = { id: 'silo', servicePassword: 'test-only' };
  const keys = await deriveKeys(options.servicePassword, options.id, 10000);
  const client = {
    ws: { readyState: 1 },
    async connect() {},
    async subscribe(topic, qos) {
      subscriptions.push([topic, qos]);
      if (topic.endsWith('/log')) throw new Error('denied');
    },
    async publish(topic, payload) {
      const req = JSON.parse(payload);
      const mid = topic.split('/').at(-1);
      await transport.receive('gm/silo/log', 'not JSON\n<script>bad()</script>\n', false);
      await transport.receive('gm/silo/log', '{"v":2,"state":"offline"}', false);
      await transport.receive('gm/silo/log', 'retained logs are ignored', true);
      const iv = 'b'.repeat(32);
      const body = Buffer.from(
        await cryptBody(
          keys.enc,
          { id: 'silo', nonce: transport.status.nonce, ...req, mid, iv },
          'res',
          new TextEncoder().encode('{"ok":true}'),
        ),
      ).toString('base64');
      const response = { v: 2, cid: req.cid, seq: req.seq, status: 200, ctype: 'application/json', iv, body };
      const mac = await hmac(
        keys.mac,
        macText('res', {
          ...response,
          id: 'silo',
          nonce: transport.status.nonce,
          mid,
        }),
      );
      client.onmessage('gm/silo/res/' + mid, JSON.stringify({ ...response, mac }), false);
    },
    close() {
      assert.fail('Optional log denial must not close the tunnel');
    },
  };
  transport = new MqttTransport(options, client);
  await transport.connect();
  assert.deepEqual(subscriptions, [
    ['gm/silo/res/#', undefined],
    ['gm/silo/status', undefined],
    ['gm/silo/log', 0],
  ]);
  assert.equal(transport.logs.state, 'denied');
  for (const lang of ['pl', 'en']) {
    assert.match(logText(lang, 'denied'), /gm\/<id>\/log/);
    assert.ok(logText(lang, 'denied').length > 30);
  }
  const status = {
    v: 2,
    state: 'online',
    nonce: 'a'.repeat(32),
    fw: 'test',
    name: 'Silo',
    until: 0,
    remaining_s: 600,
    budget_left_s: 600,
    kdf_iter: 10000,
  };
  status.mac = await hmac(keys.mac, statusMacText('silo', status));
  await transport.receive('gm/silo/status', JSON.stringify(status), true);
  assert.deepEqual(await transport.request('GET', '/api/status'), { ok: true });
  assert.equal(transport.status.state, 'online');
  assert.equal(transport.logs.lines.length, 3);
  assert.equal(transport.logs.lines[1].text, '<script>bad()</script>');
  client.onclose();
  assert.equal(transport.logs.state, 'offline');
});

test('other optional log failures are readable and reconnect retries the log subscription', async () => {
  let attempts = 0;
  const transport = new MqttTransport(
    { id: 'silo' },
    {
      ws: { readyState: 1 },
      async connect() {},
      async subscribe(topic) {
        if (topic.endsWith('/log') && !attempts++) throw new Error('timeout');
      },
    },
  );
  await transport.connect();
  assert.equal(transport.logs.state, 'error');
  await transport.connect();
  assert.equal(transport.logs.state, 'ready');
  assert.equal(attempts, 2);
});

test('network failures and closed sockets during the optional log subscription fail tunnel connection', async () => {
  for (const [message, readyState, notifyClose] of [
    ['network', 1, false],
    ['network', 3, true],
    ['timeout', 3, false],
    ['denied', 3, false],
    [null, 3, true],
  ]) {
    const client = {
      ws: { readyState: 1 },
      async connect() {},
      async subscribe(topic) {
        if (!topic.endsWith('/log')) return;
        client.ws.readyState = readyState;
        if (notifyClose) client.onclose();
        if (message) throw new Error(message);
      },
    };
    const transport = new MqttTransport({ id: 'silo' }, client);
    await assert.rejects(transport.connect(), (error) => error.message === 'network');
    assert.equal(transport.logs.state, 'offline');
  }
});
