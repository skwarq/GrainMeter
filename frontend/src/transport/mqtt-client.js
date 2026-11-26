const enc = new TextEncoder();
const dec = new TextDecoder();
const u16 = (n) => [n >> 8, n & 255];
const str = (s) => {
  const b = enc.encode(s);
  return [...u16(b.length), ...b];
};

export function encodeLength(n) {
  if (!Number.isInteger(n) || n < 0 || n > 268435455) throw new Error('length');
  const bytes = [];
  do {
    let b = n % 128;
    n = Math.floor(n / 128);
    if (n) b |= 128;
    bytes.push(b);
  } while (n);
  return bytes;
}
export function packet(header, body = []) {
  return new Uint8Array([header, ...encodeLength(body.length), ...body]);
}
export function publishPacket(topic, payload, id = 0) {
  return packet(id ? 0x32 : 0x30, [...str(topic), ...(id ? u16(id) : []), ...enc.encode(payload)]);
}
export class PacketDecoder {
  constructor() {
    this.buffer = new Uint8Array();
  }
  push(bytes) {
    this.buffer = new Uint8Array([...this.buffer, ...new Uint8Array(bytes)]);
    const packets = [];
    while (this.buffer.length >= 2) {
      let length = 0,
        multiplier = 1,
        i = 1,
        b;
      do {
        if (i >= this.buffer.length) return packets;
        b = this.buffer[i++];
        length += (b & 127) * multiplier;
        multiplier *= 128;
        if (i === 5 && b & 128) throw new Error('length');
      } while (b & 128);
      if (length > 32768) throw new Error('packet too large');
      if (this.buffer.length < i + length) break;
      packets.push({ header: this.buffer[0], body: this.buffer.slice(i, i + length) });
      this.buffer = this.buffer.slice(i + length);
    }
    return packets;
  }
}
export class MqttClient {
  constructor({ url, username, password, id, WebSocketImpl = globalThis.WebSocket }) {
    Object.assign(this, { url, username, password, id, WebSocketImpl });
    this.pending = new Map();
    this.nextId = 0;
    this.decoder = new PacketDecoder();
    this.onmessage = () => {};
    this.onclose = () => {};
  }
  allocate() {
    this.nextId = (this.nextId % 65535) + 1;
    return this.nextId;
  }
  send(bytes) {
    this.ws.send(bytes);
  }
  connect() {
    this.decoder = new PacketDecoder();
    this.pinging = false;
    return new Promise((resolve, reject) => {
      const ws = (this.ws = new this.WebSocketImpl(this.url, 'mqtt'));
      ws.binaryType = 'arraybuffer';
      const timer = setTimeout(() => {
        reject(new Error('timeout'));
        this.close();
      }, 15000);
      this.connectReject = reject;
      ws.onopen = () => {
        const random = crypto.getRandomValues(new Uint8Array(3));
        const suffix = Array.from(random, (b) => b.toString(16).padStart(2, '0')).join('');
        this.send(
          packet(0x10, [
            ...str('MQTT'),
            4,
            0xc2,
            0,
            30,
            ...str('gm-' + this.id + '-web-' + suffix),
            ...str(this.username),
            ...str(this.password),
          ]),
        );
      };
      ws.onmessage = (event) => {
        try {
          for (const p of this.decoder.push(event.data)) {
            const type = p.header >> 4,
              b = p.body;
            if (type === 2) {
              if (b.length !== 2 || b[0] !== 0 || b[1] !== 0)
                throw new Error(b[1] === 4 || b[1] === 5 ? 'auth' : 'CONNACK');
              clearTimeout(timer);
              resolve();
              this.heartbeat = setInterval(() => {
                if (this.pinging) {
                  this.close();
                  return;
                }
                this.pinging = true;
                this.send(packet(0xc0));
              }, 15000);
            } else if (type === 3) {
              const qos = (p.header >> 1) & 3;
              if (qos > 1 || b.length < 2) throw new Error('PUBLISH');
              const n = b[0] * 256 + b[1],
                pos = 2 + n;
              if (b.length < pos + (qos ? 2 : 0)) throw new Error('PUBLISH');
              if (qos) this.send(packet(0x40, b.slice(pos, pos + 2)));
              this.onmessage(dec.decode(b.slice(2, pos)), dec.decode(b.slice(pos + (qos ? 2 : 0))), !!(p.header & 1));
            } else if (type === 4 || type === 9) {
              const id = b[0] * 256 + b[1],
                pending = this.pending.get(id);
              if (pending) {
                this.pending.delete(id);
                clearTimeout(pending.timer);
                if (type !== pending.type || (type === 9 && (b.length !== 3 || ![0, 1, 2, 128].includes(b[2]))))
                  pending.reject(new Error('ACK'));
                else if (type === 9 && b[2] === 128) pending.reject(new Error('denied'));
                else pending.resolve();
              }
            } else if (type === 13) this.pinging = false;
          }
        } catch (e) {
          reject(e);
          this.close();
        }
      };
      ws.onerror = () => {
        reject(new Error('network'));
        this.close();
      };
      ws.onclose = () => {
        clearTimeout(timer);
        clearInterval(this.heartbeat);
        reject(new Error('network'));
        for (const p of this.pending.values()) {
          clearTimeout(p.timer);
          p.reject(new Error('network'));
        }
        this.pending.clear();
        this.onclose();
      };
    });
  }
  acknowledged(type, makePacket) {
    const id = this.allocate();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error('timeout'));
      }, 15000);
      this.pending.set(id, { resolve, reject, timer, type });
      try {
        this.send(makePacket(id));
      } catch (e) {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(e);
      }
    });
  }
  subscribe(topic, qos = 1) {
    return this.acknowledged(9, (id) => packet(0x82, [...u16(id), ...str(topic), qos])).catch((e) => {
      if (e.message === 'denied') e.topic = topic;
      throw e;
    });
  }
  publish(topic, payload, qos = 1) {
    if (!qos) {
      this.send(publishPacket(topic, payload));
      return Promise.resolve();
    }
    return this.acknowledged(4, (id) => publishPacket(topic, payload, id));
  }
  close() {
    clearInterval(this.heartbeat);
    if (this.ws?.readyState === 1) this.send(packet(0xe0));
    this.ws?.close();
  }
}
