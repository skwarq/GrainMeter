import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MqttClient, PacketDecoder, packet, publishPacket } from '../src/transport/mqtt-client.js';
import { MqttTransport } from '../src/transport/mqtt.js';

class WS {
  readyState = 1;
  sent = [];
  constructor() {
    WS.instance = this;
  }
  send(bytes) {
    this.sent.push(bytes);
  }
  close() {
    this.readyState = 3;
    this.onclose?.();
  }
  receive(bytes) {
    this.onmessage({ data: bytes.buffer });
  }
}

test('tunnel connect rejects when WebSocket closes before optional log SUBACK', async () => {
  const options = { url: 'wss://test/mqtt', id: 'silo', username: 'web', password: 'test', WebSocketImpl: WS };
  const client = new MqttClient(options);
  const transport = new MqttTransport(options, client);
  const connection = transport.connect();
  const rejected = assert.rejects(connection, (e) => e.message === 'network');
  const ws = WS.instance;
  const waitSent = async (count) => {
    for (let i = 0; i < 20 && ws.sent.length < count; i++) await Promise.resolve();
    assert.equal(ws.sent.length, count);
  };
  try {
    ws.onopen();
    ws.receive(packet(0x20, [0, 0]));
    await waitSent(2);
    ws.receive(packet(0x90, [0, 1, 1]));
    await waitSent(3);
    ws.receive(packet(0x90, [0, 2, 1]));
    await waitSent(4);
    assert.equal(transport.logs.state, 'subscribing');
    ws.close();
    await rejected;
    assert.equal(transport.logs.state, 'offline');
    assert.equal(client.pending.size, 0);
  } finally {
    client.close();
  }
});

test('log SUBACK denial rejects only its subscription; response subscription and request PUBACK still work', async () => {
  const client = new MqttClient({
    url: 'wss://test/mqtt',
    id: 'silo',
    username: 'web',
    password: 'test',
    WebSocketImpl: WS,
  });
  const connection = client.connect();
  const ws = WS.instance;
  ws.onopen();
  ws.receive(packet(0x20, [0, 0]));
  await connection;
  try {
    const responses = client.subscribe('gm/silo/res/#');
    const logs = client.subscribe('gm/silo/log', 0);
    const denied = assert.rejects(logs, (e) => e.message === 'denied' && e.topic === 'gm/silo/log');
    const logPacket = new PacketDecoder().push(ws.sent.at(-1))[0];
    assert.equal(logPacket.body.at(-1), 0);
    ws.receive(packet(0x90, [0, 2, 128]));
    await denied;
    assert.equal(ws.readyState, 1);
    ws.receive(packet(0x90, [0, 1, 1]));
    await responses;
    const request = client.publish('gm/silo/req/a', '{}');
    ws.receive(packet(0x40, [0, 3]));
    await request;
    let received;
    client.onmessage = (...args) => {
      received = args;
    };
    ws.receive(publishPacket('gm/silo/res/a', '{"ok":true}'));
    assert.deepEqual(received, ['gm/silo/res/a', '{"ok":true}', false]);
    assert.equal(client.pending.size, 0);
  } finally {
    client.close();
  }
});

test('QoS0 SUBACK grants succeed; malformed acknowledgements are not reported as ACL denial', async () => {
  const client = new MqttClient({
    url: 'wss://test/mqtt',
    id: 'silo',
    username: 'web',
    password: 'test',
    WebSocketImpl: WS,
  });
  const connection = client.connect();
  const ws = WS.instance;
  ws.onopen();
  ws.receive(packet(0x20, [0, 0]));
  await connection;
  try {
    const granted = client.subscribe('gm/silo/log', 0);
    ws.receive(packet(0x90, [0, 1, 0]));
    await granted;
    const malformed = client.subscribe('gm/silo/log', 0);
    const rejected = assert.rejects(malformed, (e) => e.message === 'ACK' && !e.topic);
    ws.receive(packet(0x90, [0, 2, 99]));
    await rejected;
    assert.equal(ws.readyState, 1);
  } finally {
    client.close();
  }
});
