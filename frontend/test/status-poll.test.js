import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createPoll } from '../src/lib/poll.js';
import { getStatus, setRemoteTransport } from '../src/api.js';
import { build } from 'esbuild';
import { translate } from '../src/i18n/translate.js';

const flush = async () => {
  for (let i = 0; i < 20; i++) await Promise.resolve();
};

function fixture(t, { hidden = false, delay = 0 } = {}) {
  t.mock.timers.enable({ apis: ['Date', 'setTimeout'], now: 1000 });
  const doc = new EventTarget();
  doc.hidden = hidden;
  let requests = 0;
  let inFlight = 0;
  let maxInFlight = 0;
  const updates = [];
  const transport = {
    request(method, path, { signal }) {
      assert.equal(method, 'GET');
      assert.equal(path, '/api/status');
      requests++;
      maxInFlight = Math.max(maxInFlight, ++inFlight);
      return new Promise((resolve, reject) => {
        const finish = () => {
          inFlight--;
          if (signal.aborted) reject({ kind: 'aborted' });
          else resolve({ name: 'Test', firmware: String(requests) });
        };
        if (delay) setTimeout(finish, delay);
        else finish();
      });
    },
  };
  setRemoteTransport(transport);
  const poll = createPoll(getStatus, (s) => updates.push(s), doc);
  t.after(() => {
    poll.stop();
    setRemoteTransport(null);
  });
  const advance = async (ms) => {
    t.mock.timers.tick(ms);
    await flush();
  };
  const visible = async (value) => {
    doc.hidden = !value;
    doc.dispatchEvent(new Event('visibilitychange'));
    await flush();
  };
  return { poll, doc, updates, advance, visible, counts: () => ({ requests, inFlight, maxInFlight }) };
}

test('20 seconds of overview, page switch and action: 6 polls, no remount poll, one shared refresh', async (t) => {
  const f = fixture(t);
  f.poll.start(4000);
  await flush();
  for (let i = 0; i < 5; i++) await f.advance(4000);
  assert.equal(f.counts().requests, 6); // t=0,4,8,12,16,20 s
  // Page consumers change, but the App-owned poll remains mounted.
  await f.advance(1000);
  assert.equal(f.counts().requests, 6);
  const action = f.poll.refresh();
  assert.equal(f.poll.refresh(), action);
  assert.equal(f.poll.refresh(), action);
  assert.equal((await action).firmware, '7');
  await f.advance(3000); // The replaced t=24 cycle must not fire.
  assert.equal(f.counts().requests, 7);
  await f.advance(1000);
  assert.deepEqual(f.counts(), { requests: 8, inFlight: 0, maxInFlight: 1 });
});

test('slow status: manual refresh and visibility join in flight; next poll waits for completion', async (t) => {
  const f = fixture(t, { delay: 6000 });
  f.poll.start(4000);
  await flush();
  const pending = f.poll.refresh();
  await f.advance(4000);
  await f.visible(false);
  await f.visible(true);
  assert.equal(f.poll.refresh(), pending);
  assert.equal(f.counts().requests, 1);
  await f.advance(2000);
  assert.ok(await pending);
  await f.advance(3999);
  assert.equal(f.counts().requests, 1);
  await f.advance(1);
  assert.deepEqual(f.counts(), { requests: 2, inFlight: 1, maxInFlight: 1 });
});

test('120 ms tunnel latency: 5 requests in 20 seconds, then one coalesced action refresh', async (t) => {
  const f = fixture(t, { delay: 120 });
  f.poll.start(4000);
  await flush();
  await f.advance(120);
  for (let i = 0; i < 4; i++) {
    await f.advance(4000);
    await f.advance(120);
  }
  await f.advance(3400);
  assert.equal(f.counts().requests, 5);
  const action = f.poll.refresh();
  assert.equal(f.poll.refresh(), action);
  await flush();
  await f.advance(120);
  assert.ok(await action);
  assert.deepEqual(f.counts(), { requests: 6, inFlight: 0, maxInFlight: 1 });
});

test('hidden mount and background pause; return triggers exactly one request', async (t) => {
  const f = fixture(t, { hidden: true });
  f.poll.start(4000);
  await f.advance(30000);
  assert.equal(f.counts().requests, 0);
  await f.visible(true);
  assert.equal(f.counts().requests, 1);
  await f.visible(false);
  await f.advance(30000);
  assert.equal(f.counts().requests, 1);
  await f.visible(true);
  assert.equal(f.counts().requests, 2);
});

test('cleanup during a request leaves no zombie timer or state update', async (t) => {
  const f = fixture(t, { delay: 6000 });
  f.poll.start(4000);
  await flush();
  f.poll.stop();
  await f.advance(6000);
  await f.advance(30000);
  assert.equal(f.counts().requests, 1);
  assert.equal(f.updates.length, 0);
  assert.equal(await f.poll.refresh(), null);
});

test('effect restart waits for an aborted request, then uses the new interval without overlap', async (t) => {
  const f = fixture(t, { delay: 6000 });
  f.poll.start(4000);
  await flush();
  f.poll.stop();
  f.poll.start(1000);
  await flush();
  assert.equal(f.counts().requests, 1);
  await f.advance(6000);
  assert.deepEqual(f.counts(), { requests: 2, inFlight: 1, maxInFlight: 1 });
  assert.equal(f.updates.length, 0);
  await f.advance(6000);
  assert.equal(f.updates.length, 1);
  await f.advance(1000);
  assert.equal(f.counts().requests, 3);
});

test('errors remain visible and refresh recovers; OTA callers share the result', async (t) => {
  const f = fixture(t);
  const error = { kind: 'network' };
  setRemoteTransport({ request: () => Promise.reject(error) });
  f.poll.start(4000);
  assert.equal(await f.poll.refresh(), null);
  assert.deepEqual(f.updates, [{ error }]);
  setRemoteTransport({ request: () => Promise.resolve({ firmware: 'new' }) });
  const ota = f.poll.refresh();
  assert.equal(f.poll.refresh(), ota);
  assert.deepEqual(await ota, { firmware: 'new' });
  assert.equal(f.updates.at(-1).error, null);
});

// Minimal DOM for mounting the real Preact App/hooks/pages without adding a
// browser dependency. Events use Node's EventTarget; no layout is needed.
class Node extends EventTarget {
  constructor(type, name, data = '') {
    super();
    this.nodeType = type;
    this.localName = name;
    this.data = data;
    this.childNodes = [];
    this.parentNode = null;
    this.style = { setProperty() {} };
    this.attributes = [];
  }
  get firstChild() {
    return this.childNodes[0] || null;
  }
  get nextSibling() {
    if (!this.parentNode) return null;
    return this.parentNode.childNodes[this.parentNode.childNodes.indexOf(this) + 1] || null;
  }
  appendChild(node) {
    return this.insertBefore(node, null);
  }
  insertBefore(node, before) {
    node.parentNode?.removeChild(node);
    const index = before ? this.childNodes.indexOf(before) : this.childNodes.length;
    this.childNodes.splice(index, 0, node);
    node.parentNode = this;
    return node;
  }
  removeChild(node) {
    this.childNodes.splice(this.childNodes.indexOf(node), 1);
    node.parentNode = null;
  }
  remove() {
    this.parentNode?.removeChild(this);
  }
  setAttribute(name, value) {
    this.removeAttribute(name);
    this.attributes.push({ name, value: String(value) });
  }
  removeAttribute(name) {
    this.attributes = this.attributes.filter((a) => a.name !== name);
  }
}

const bundle = await build({
  stdin: {
    contents: `export { App } from './src/app.jsx';
      export { SessionBar } from './src/remote/SessionBar.jsx';
      export { setRemoteTransport } from './src/api.js';
      export { render, h } from 'preact';
      export { act } from 'preact/test-utils';`,
    resolveDir: new URL('..', import.meta.url).pathname,
  },
  bundle: true,
  write: false,
  format: 'esm',
  platform: 'node',
  jsx: 'automatic',
  jsxImportSource: 'preact',
  loader: { '.css': 'empty' },
});
const ui = await import('data:text/javascript;base64,' + Buffer.from(bundle.outputFiles[0].text).toString('base64'));

test('real App + Overview + SessionBar: navigation and session rerenders do not create pollers', async (t) => {
  t.mock.timers.enable({ apis: ['Date', 'setTimeout'], now: 1000 });
  // Countdown rerenders are exercised explicitly; their intervals are not
  // part of status polling and would interfere with Node's mocked timeouts.
  t.mock.method(globalThis, 'setInterval', () => 0);
  t.mock.method(globalThis, 'clearInterval', () => {});
  const doc = new EventTarget();
  Object.assign(doc, {
    hidden: false,
    documentElement: {},
    createElement: (name) => new Node(1, name),
    createElementNS: (_, name) => new Node(1, name),
    createTextNode: (data) => new Node(3, '', data),
  });
  const globals = { document: doc, window: new EventTarget(), location: { hash: '#/overview' } };
  const previous = Object.fromEntries(
    Object.keys(globals).map((key) => [key, Object.getOwnPropertyDescriptor(globalThis, key)]),
  );
  Object.assign(globalThis, globals);
  const root = new Node(1, 'div');
  let requests = 0;
  let actions = 0;
  let finishRestart;
  ui.setRemoteTransport({
    request: async (method, path) => {
      if (method === 'POST') {
        if (path === '/api/restart') return new Promise((resolve) => (finishRestart = resolve));
        assert.equal(path, '/api/remote/extend');
        actions++;
        return { remaining_s: 1200, budget_left_s: 600 };
      }
      assert.equal(method, 'GET');
      // SessionBar reads remote_idle_min once (idle warning); not a status poll.
      if (path === '/api/config') return { remote_idle_min: 10 };
      assert.equal(path, '/api/status');
      requests++;
      return { name: 'Test', firmware: '1', remote: { remaining_s: 600, budget_left_s: 600 } };
    },
  });
  const tree = (remaining = 600) =>
    ui.h(
      ui.App,
      null,
      ui.h(ui.SessionBar, {
        online: true,
        session: { deadline: Date.now() + remaining * 1000 },
        lang: 'pl',
      }),
    );
  t.after(async () => {
    await ui.act(() => ui.render(null, root));
    ui.setRemoteTransport(null);
    for (const [key, descriptor] of Object.entries(previous)) {
      if (descriptor) Object.defineProperty(globalThis, key, descriptor);
      else delete globalThis[key];
    }
  });
  await ui.act(async () => {
    ui.render(tree(), root);
    await flush();
  });
  await flush();
  assert.equal(requests, 1);
  for (let i = 0; i < 5; i++) {
    await ui.act(async () => {
      t.mock.timers.tick(4000);
      await flush();
    });
  }
  assert.equal(requests, 6);
  await ui.act(() => {
    globalThis.location.hash = '#/service';
    globalThis.window.dispatchEvent(new Event('hashchange'));
    ui.render(tree(500), root);
  });
  assert.equal(requests, 6);
  const find = (node, match) => {
    if (match(node)) return node;
    for (const child of node.childNodes) {
      const found = find(child, match);
      if (found) return found;
    }
    return null;
  };
  const bar = find(root, (node) => node.attributes.some((a) => a.value === 'remote-session'));
  const extend = find(bar, (node) => node.localName === 'button');
  await ui.act(async () => {
    extend.dispatchEvent(new Event('click'));
    await flush();
  });
  assert.equal(actions, 1);
  // The extend reply carries the new deadline: no extra status request.
  assert.equal(requests, 6);
  await ui.act(async () => {
    t.mock.timers.tick(4000);
    await flush();
  });
  assert.equal(requests, 7);
  const text = (node) => node.data + node.childNodes.map(text).join('');
  const navigate = async (page) =>
    ui.act(() => {
      globalThis.location.hash = '#/' + page;
      globalThis.window.dispatchEvent(new Event('hashchange'));
    });
  for (const lateReply of [false, true]) {
    const before = requests;
    await ui.act(() => t.mock.timers.tick(1000));
    await navigate('service');
    const click = async (key) =>
      ui.act(async () => {
        const button = find(root, (node) => node.localName === 'button' && text(node) === translate('pl', key));
        assert.ok(button, key);
        button.dispatchEvent(new Event('click'));
        await flush();
      });
    await click('service.restart');
    await click('common.confirm');
    if (!lateReply) {
      await ui.act(async () => {
        finishRestart({ restarting: true });
        await flush();
      });
    }
    await navigate('overview');
    if (lateReply) {
      await ui.act(async () => {
        finishRestart({ restarting: true });
        await flush();
      });
    }
    await ui.act(async () => {
      t.mock.timers.tick(3000);
      await flush();
    });
    await ui.act(async () => {
      t.mock.timers.tick(1000);
      await flush();
    });
    assert.equal(requests, before + 1, 'leaving Service cancels both scheduled and late restart refreshes');
  }
});
