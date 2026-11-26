// SPDX-License-Identifier: MIT
#include "app/portal_tunnel.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/base64url.h"
#include "util/hmac_sha256.h"
namespace gm {
namespace {
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
// Valid UTF-8 scalar length; rejects overlong encodings and surrogates.
size_t utf8(const char* p, size_t n) {
  const unsigned char c = static_cast<unsigned char>(*p);
  if (c < 128) return 1;
  size_t k = c >= 0xc2 && c <= 0xdf   ? 2
             : c >= 0xe0 && c <= 0xef ? 3
             : c >= 0xf0 && c <= 0xf4 ? 4
                                      : 0;
  if (!k || n < k) return 0;
  for (size_t i = 1; i < k; ++i)
    if ((static_cast<unsigned char>(p[i]) & 0xc0) != 0x80) return 0;
  const unsigned char d = static_cast<unsigned char>(p[1]);
  if ((c == 0xe0 && d < 0xa0) || (c == 0xed && d >= 0xa0) ||
      (c == 0xf0 && d < 0x90) || (c == 0xf4 && d >= 0x90))
    return 0;
  return k;
}
// The existing manifest reader does not validate whole documents or support
// Unicode escapes. This bounded flat reader deliberately accepts only gm2
// fields.
struct Reader {
  const char* p;
  const char* end;
  void ws() {
    while (p != end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
      ++p;
  }
  bool take(char c) {
    ws();
    if (p == end || *p != c) return false;
    ++p;
    return true;
  }
  bool code(unsigned* u) {
    *u = 0;
    for (int i = 0; i < 4; ++i) {
      if (p == end || hex(*p) < 0) return false;
      *u = (*u << 4) | static_cast<unsigned>(hex(*p++));
    }
    return true;
  }
  bool str(char* out, size_t cap) {
    if (!take('"')) return false;
    size_t n = 0;
    while (p != end && *p != '"') {
      char bytes[4];
      size_t k = 1;
      unsigned char c = static_cast<unsigned char>(*p++);
      if (c < 32) return false;
      if (c == '\\') {
        if (p == end) return false;
        c = static_cast<unsigned char>(*p++);
        if (c == 'u') {
          unsigned u;
          if (!code(&u)) return false;
          if (u >= 0xd800 && u <= 0xdbff) {
            if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return false;
            p += 2;
            unsigned lo;
            if (!code(&lo) || lo < 0xdc00 || lo > 0xdfff) return false;
            u = 0x10000 + ((u - 0xd800) << 10) + lo - 0xdc00;
          } else if (u >= 0xdc00 && u <= 0xdfff)
            return false;
          if (!u) return false;  // Portal fields are C strings.
          if (u < 128)
            bytes[0] = static_cast<char>(u);
          else if (u < 2048) {
            k = 2;
            bytes[0] = static_cast<char>(0xc0 | (u >> 6));
            bytes[1] = static_cast<char>(0x80 | (u & 63));
          } else if (u < 65536) {
            k = 3;
            bytes[0] = static_cast<char>(0xe0 | (u >> 12));
            bytes[1] = static_cast<char>(0x80 | ((u >> 6) & 63));
            bytes[2] = static_cast<char>(0x80 | (u & 63));
          } else {
            k = 4;
            bytes[0] = static_cast<char>(0xf0 | (u >> 18));
            bytes[1] = static_cast<char>(0x80 | ((u >> 12) & 63));
            bytes[2] = static_cast<char>(0x80 | ((u >> 6) & 63));
            bytes[3] = static_cast<char>(0x80 | (u & 63));
          }
        } else {
          switch (c) {
            case '"':
            case '\\':
            case '/':
              break;
            case 'b':
              c = '\b';
              break;
            case 'f':
              c = '\f';
              break;
            case 'n':
              c = '\n';
              break;
            case 'r':
              c = '\r';
              break;
            case 't':
              c = '\t';
              break;
            default:
              return false;
          }
          bytes[0] = static_cast<char>(c);
        }
      } else {
        --p;
        k = utf8(p, static_cast<size_t>(end - p));
        if (!k) return false;
        memcpy(bytes, p, k);
        p += k;
      }
      if (n + k >= cap) return false;
      memcpy(out + n, bytes, k);
      n += k;
    }
    if (p == end) return false;
    ++p;
    out[n] = 0;
    return true;
  }
  bool num(uint64_t* out) {
    ws();
    if (p == end || *p < '0' || *p > '9') return false;
    bool zero = *p == '0';
    *out = 0;
    unsigned digits = 0;
    while (p != end && *p >= '0' && *p <= '9') {
      unsigned d = static_cast<unsigned>(*p++ - '0');
      if (*out > (UINT64_MAX - d) / 10 || (zero && digits)) return false;
      *out = *out * 10 + d;
      ++digits;
    }
    return true;
  }
};
struct Request {
  uint64_t seq, version;
  char method[9], cid[17], iv[33], mac[65];
  char* path;
  char* body;
  bool parse(const char* p, size_t n) {
    Reader r = {p, p + n};
    if (!r.take('{')) return false;
    unsigned seen = 0;
    do {
      char key[16];
      if (!r.str(key, sizeof(key)) || !r.take(':')) return false;
      unsigned bit = 0;
      bool ok = false;
      if (!strcmp(key, "v")) {
        bit = 1;
        ok = r.num(&version);
      } else if (!strcmp(key, "seq")) {
        bit = 2;
        ok = r.num(&seq);
      } else if (!strcmp(key, "method")) {
        bit = 4;
        ok = r.str(method, sizeof(method));
      } else if (!strcmp(key, "path")) {
        bit = 8;
        ok = r.str(path, 257);
      } else if (!strcmp(key, "body")) {
        bit = 16;
        ok = r.str(body, 2733);
      } else if (!strcmp(key, "mac")) {
        bit = 32;
        ok = r.str(mac, sizeof(mac));
      } else if (!strcmp(key, "cid")) {
        bit = 64;
        ok = r.str(cid, sizeof(cid));
      } else if (!strcmp(key, "iv")) {
        bit = 128;
        ok = r.str(iv, sizeof(iv));
      }
      if (!ok || (seen & bit)) return false;
      seen |= bit;
    } while (r.take(','));
    if (!r.take('}')) return false;
    r.ws();
    if (strlen(cid) != 16 || strlen(iv) != 32) return false;
    const char* hexFields[] = {cid, iv, mac};
    for (const char* s : hexFields)
      for (; *s; ++s)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f')))
          return false;
    const size_t bodyLen = strlen(body);
    if (bodyLen % 4) return false;
    for (size_t i = 0; i < bodyLen; ++i)
      if (!((body[i] >= 'A' && body[i] <= 'Z') ||
            (body[i] >= 'a' && body[i] <= 'z') ||
            (body[i] >= '0' && body[i] <= '9') || body[i] == '+' ||
            body[i] == '/' || body[i] == '='))
        return false;
    return r.p == r.end && seen == 255 && version == 2 && strlen(mac) == 64 &&
           seq >= 1 && seq <= 9007199254740991ULL &&
           (!strcmp(method, "GET") || !strcmp(method, "POST")) &&
           path[0] == '/' && !strchr(path, '?') && !strchr(path, '\n') &&
           !strchr(path, '\r');
  }
};
// Reads a page repeats on its own (status every few seconds, the log view,
// history refresh) do not count as activity for the remote idle timeout:
// a forgotten browser tab must not keep the radio on until the session end.
bool backgroundPoll(const char* method, const char* path) {
  return !strcmp(method, "GET") &&
         (!strcmp(path, "/api/status") || !strncmp(path, "/api/logs/", 10) ||
          !strncmp(path, "/api/history", 12));
}
bool validMid(const char* mid) {
  if (!mid) return false;
  size_t n = 0;
  for (; mid[n]; ++n) {
    char c = mid[n];
    if (n == 16 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  return n != 0;
}
void sign(const uint8_t key[32], const char* const* parts, size_t count,
          char out[65]) {
  HmacSha256 h(key, 32);
  for (size_t i = 0; i < count; ++i) {
    if (i) h.update("\n", 1);
    h.update(parts[i], strlen(parts[i]));
  }
  uint8_t digest[32];
  h.finish(digest);
  sha256ToHex(digest, out);
}
uint32_t retiredHash(const char* cid, uint32_t seed) {
  for (; *cid; ++cid) seed = (seed ^ static_cast<uint8_t>(*cid)) * 16777619u;
  return seed % 1024;
}
struct Writer {
  char* p;
  size_t n, cap;
  bool ok;
  void add(const char* s, size_t len) {
    if (n + len >= cap) {
      ok = false;
      return;
    }
    memcpy(p + n, s, len);
    n += len;
    p[n] = 0;
  }
  void text(const char* s) { add(s, strlen(s)); }
  void string(const char* s, size_t len) {
    text("\"");
    for (size_t i = 0; i < len;) {
      unsigned char c = static_cast<unsigned char>(s[i]);
      size_t k = utf8(s + i, len - i);
      if (c == '"' || c == '\\') {
        text("\\");
        add(s + i, 1);
        ++i;
      } else if (c < 32 || !k) {
        char esc[7];
        snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(c));
        text(esc);
        ++i;
      } else {
        add(s + i, k);
        i += k;
      }
    }
    text("\"");
  }
};
struct CachedEnvelope {
  uint32_t len;
  uint32_t flags;  // client bits 0..1, restart bit 2, status bits 8..23
};
}  // namespace
PortalTunnel::PortalTunnel(Portal& portal, GrainMeter& app, size_t limit)
    : portal_(portal),
      app_(app),
      limit_(limit < kMaxResponse ? limit : static_cast<size_t>(kMaxResponse)),
      age_(0),
      ivCounter_(0),
      cacheClient_(-1),
      failures_(0),
      lockStart_(0),
      locked_(false),
      busy_(false),
      session_(false),
      cacheLen_(0),
      cacheStatus_(0),
      cacheRestart_(false) {
  nonce_[0] = id_[0] = password_[0] = 0;
  memset(clients_, 0, sizeof(clients_));
  memset(retired_, 0, sizeof(retired_));
}
void PortalTunnel::beginSession(const char* nonce, const char* id) {
  session_ =
      nonce && id && strlen(nonce) == 32 && strlen(id) > 0 && strlen(id) <= 24;
  if (session_) {
    for (size_t i = 0; i < 32; ++i)
      if (!((nonce[i] >= '0' && nonce[i] <= '9') ||
            (nonce[i] >= 'a' && nonce[i] <= 'f')))
        session_ = false;
    for (size_t i = 0; id[i]; ++i)
      if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') ||
            id[i] == '-'))
        session_ = false;
  }
  if (session_) {
    strcpy(nonce_, nonce);
    strcpy(id_, id);
  }
  memset(clients_, 0, sizeof(clients_));
  memset(retired_, 0, sizeof(retired_));
  age_ = ivCounter_ = 0;
  cacheClient_ = -1;
  failures_ = 0;
  locked_ = false;
  cacheLen_ = 0;
  // A new session always derives, even if the password has not changed.
  snprintf(password_, sizeof(password_), "%s", app_.config().servicePassword);
  if (session_) gm2DeriveKeys(password_, id_, macKey_, encKey_);
}
bool PortalTunnel::refreshKeys() {
  if (!session_ || !strcmp(password_, app_.config().servicePassword))
    return false;
  snprintf(password_, sizeof(password_), "%s", app_.config().servicePassword);
  gm2DeriveKeys(password_, id_, macKey_, encKey_);
  cacheLen_ = 0;
  cacheClient_ = -1;
  return true;
}
bool PortalTunnel::readCache(int client, TunnelOut* out) {
  for (size_t offset = 0; offset < cacheLen_;) {
    CachedEnvelope entry;
    memcpy(&entry, cache_ + offset, sizeof(entry));
    if (static_cast<int>(entry.flags & 3) == client) {
      memcpy(output_, cache_ + offset + sizeof(entry), entry.len + 1);
      cacheStatus_ = static_cast<int>(entry.flags >> 8);
      cacheRestart_ = (entry.flags & 4) != 0;
      *out = {output_, entry.len, cacheRestart_, cacheStatus_};
      return true;
    }
    offset += sizeof(entry) + entry.len + 1;
  }
  return false;
}
void PortalTunnel::eraseCache(int client) {
  for (size_t offset = 0; offset < cacheLen_;) {
    CachedEnvelope entry;
    memcpy(&entry, cache_ + offset, sizeof(entry));
    const size_t size = sizeof(entry) + entry.len + 1;
    if (static_cast<int>(entry.flags & 3) == client) {
      memmove(cache_ + offset, cache_ + offset + size,
              cacheLen_ - offset - size);
      cacheLen_ -= size;
      return;
    }
    offset += size;
  }
}
void PortalTunnel::storeCache(int client, const TunnelOut& out) {
  eraseCache(client);
  const size_t size = sizeof(CachedEnvelope) + out.len + 1;
  if (size > sizeof(cache_)) return;
  while (cacheLen_ + size > sizeof(cache_)) {
    int victim = -1;
    for (size_t offset = 0; offset < cacheLen_;) {
      CachedEnvelope entry;
      memcpy(&entry, cache_ + offset, sizeof(entry));
      const int candidate = static_cast<int>(entry.flags & 3);
      if (victim < 0 || clients_[candidate].age < clients_[victim].age)
        victim = candidate;
      offset += sizeof(entry) + entry.len + 1;
    }
    eraseCache(victim);
  }
  cacheStatus_ = out.status;
  cacheRestart_ = out.restart;
  const CachedEnvelope entry = {static_cast<uint32_t>(out.len),
                                static_cast<uint32_t>(client) |
                                    (cacheRestart_ ? 4u : 0u) |
                                    (static_cast<uint32_t>(cacheStatus_) << 8)};
  memcpy(cache_ + cacheLen_, &entry, sizeof(entry));
  memcpy(cache_ + cacheLen_ + sizeof(entry), out.payload, out.len + 1);
  cacheLen_ += size;
}
void PortalTunnel::statusMac(const char* fw, const char* name, uint32_t until,
                             uint32_t remaining, uint32_t budget,
                             const char* opMid, const char* opFw,
                             const char* opResult, char out[65]) {
  out[0] = 0;
  if (!session_ || !password_[0] || !app_.config().servicePassword[0]) return;
  // Status generation is serialized with requests; reuse the output buffer
  // rather than adding a large stack allocation for canonical JSON escaping.
  JsonWriter w(output_, sizeof(output_));
  w.beginArray()
      .unum(2)
      .str(id_)
      .str("online")
      .str(nonce_)
      .str(fw)
      .str(name)
      .unum(until)
      .unum(remaining)
      .unum(budget)
      .unum(kdfIterations());
  if (opMid && opMid[0])
    w.beginArray()
        .str(opMid)
        .str(opFw ? opFw : "")
        .str(opResult ? opResult : "")
        .endArray();
  else
    w.null();
  w.endArray();
  if (!w.ok()) return;
  HmacSha256 h(macKey_, 32);
  h.update("gm2status\n", 10);
  h.update(output_, strlen(output_));
  uint8_t digest[32];
  h.finish(digest);
  sha256ToHex(digest, out);
}
void PortalTunnel::respond(const char* mid, const char* cid, uint64_t seq,
                           int status, const char* ctype, const char* body,
                           size_t len, bool restart, const char* key,
                           TunnelOut* out) {
  char signature[65] = "", number[160], iv[33] = "";
  const bool binary = !strcmp(ctype, "application/octet-stream");
  if (key) {
    if (ivCounter_ == UINT64_MAX) {
      session_ = false;
      key = nullptr;
    } else {
      ++ivCounter_;
      snprintf(iv, sizeof(iv), "%032llx",
               static_cast<unsigned long long>(ivCounter_));
    }
  }
  Writer w = {output_, 0, sizeof(output_), true};
  snprintf(number, sizeof(number),
           "{\"v\":2,\"cid\":\"%s\",\"seq\":%llu,\"status\":%d,\"ctype\":",
           cid ? cid : "", static_cast<unsigned long long>(seq), status);
  w.text(number);
  w.string(ctype, strlen(ctype));
  if (binary) w.text(",\"enc\":\"base64\"");
  w.text(",\"iv\":");
  w.string(iv, strlen(iv));
  w.text(",\"body\":");
  if (key) {
    w.text("\"");
    const size_t bodyStart = w.n;
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t off = 0; off < len && w.ok;) {
      uint8_t bytes[96];
      size_t n = len - off;
      if (n > sizeof(bytes)) n = sizeof(bytes);
      memcpy(bytes, body + off, n);
      gm2StreamXor(encKey_, id_, nonce_, cid, seq, mid, "res", iv, bytes, n,
                   off);
      for (size_t i = 0; i < n; i += 3) {
        uint8_t a = bytes[i], b = i + 1 < n ? bytes[i + 1] : 0,
                c = i + 2 < n ? bytes[i + 2] : 0;
        const char chunk[] = {
            alphabet[a >> 2], alphabet[((a & 3) << 4) | (b >> 4)],
            i + 1 < n ? alphabet[((b & 15) << 2) | (c >> 6)] : '=',
            i + 2 < n ? alphabet[c & 63] : '='};
        w.add(chunk, 4);
      }
      off += n;
    }
    // Temporarily terminate the ciphertext for the canonical MAC.
    char seqText[24], statusText[16];
    snprintf(seqText, sizeof(seqText), "%llu",
             static_cast<unsigned long long>(seq));
    snprintf(statusText, sizeof(statusText), "%d", status);
    const char* parts[] = {"gm2res",
                           id_,
                           nonce_,
                           cid,
                           seqText,
                           mid,
                           statusText,
                           ctype,
                           binary ? "base64" : "",
                           restart ? "1" : "0",
                           iv,
                           output_ + bodyStart};
    if (w.ok) sign(macKey_, parts, 12, signature);
    w.text("\"");
  } else {
    w.string(body, len);
  }
  w.text(",\"mac\":");
  w.string(signature, strlen(signature));
  w.text(restart ? ",\"restart\":true}" : ",\"restart\":false}");
  // The fallback must remain deliverable even with a deliberately tiny limit.
  if ((!w.ok || w.n > limit_) && status != 413) {
    const char* error = "{\"error\":\"too_large\"}";
    respond(mid, cid, seq, 413, "application/json", error, strlen(error), false,
            key, out);
    return;
  }
  *out = {output_, w.n, restart, status};
}
bool PortalTunnel::handle(const char* mid, const char* payload, size_t len,
                          bool retained, uint32_t now, TunnelOut* out) {
  if (!out) return false;
  Request req = {};
  req.path = requestPath_;
  req.body = requestBody_;
  bool valid = payload && len <= 4096 && validMid(mid) && session_ &&
               req.parse(payload, len);
  // Retained publications are never answered, including malformed ones.
  if (retained) return false;
  const int plainLen =
      valid ? base64urlDecode(req.body, strlen(req.body),
                              reinterpret_cast<uint8_t*>(plaintext_),
                              kPortalMaxBody)
            : -1;
  valid = valid && plainLen >= 0;
  const char* error = nullptr;
  int status = 0;
  bool signedReply = false, consume = false;
  char body[128], key[65];
  // A LAN change may occur between transport status publications and requests.
  refreshKeys();
  snprintf(key, sizeof(key), "%s", password_);
  if (!valid) {
    status = 400;
    error = "bad_request";
  } else if (key[0] == '\0') {
    // An empty key gives a MAC anyone can compute: refuse the tunnel
    // until a service password is set over USB/LAN.
    status = 403;
    error = "no_password";
  } else if (locked_ && static_cast<uint32_t>(now - lockStart_) < 60) {
    snprintf(
        body, sizeof(body), "{\"error\":\"locked\",\"retry_s\":%u}",
        static_cast<unsigned>(60 - static_cast<uint32_t>(now - lockStart_)));
    respond(mid, req.cid, req.seq, 429, "application/json", body, strlen(body),
            false, nullptr, out);
    return true;
  } else {
    if (locked_) {
      locked_ = false;
      failures_ = 0;
    }
    char expected[65];
    char seqText[24];
    snprintf(seqText, sizeof(seqText), "%llu",
             static_cast<unsigned long long>(req.seq));
    const char* parts[] = {"gm2req", id_,        nonce_,   req.cid, seqText,
                           mid,      req.method, req.path, req.iv,  req.body};
    sign(macKey_, parts, 10, expected);
    if (!constantTimeEqual(expected, req.mac, 64)) {
      if (++failures_ >= 5) {
        locked_ = true;
        lockStart_ = now;
        failures_ = 0;
      }
      status = 401;
      error = "bad_mac";
    } else {
      failures_ = 0;
      signedReply = true;
      if (app_.ports().remote && !backgroundPoll(req.method, req.path))
        app_.ports().remote->validRequest();
      int client = -1;
      for (int i = 0; i < 4; ++i)
        if (!strcmp(clients_[i].cid, req.cid)) client = i;
      if (client < 0) {
        bool retired = true;
        const uint32_t seeds[] = {2166136261u, 314159265u, 271828183u};
        for (uint32_t seed : seeds) {
          uint32_t bit = retiredHash(req.cid, seed);
          if (!(retired_[bit / 8] & (1u << (bit % 8)))) retired = false;
        }
        if (retired) {
          const char* retiredBody = "{\"error\":\"client_retired\"}";
          respond(mid, req.cid, req.seq, 409, "application/json", retiredBody,
                  strlen(retiredBody), false, key, out);
          return true;
        }
        client = 0;
        for (int i = 0; i < 4; ++i) {
          if (!clients_[i].cid[0]) {
            client = i;
            break;
          }
          if (clients_[i].age < clients_[client].age) client = i;
        }
        if (clients_[client].cid[0]) {
          for (uint32_t seed : seeds) {
            uint32_t bit = retiredHash(clients_[client].cid, seed);
            retired_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
          }
          eraseCache(client);
        }
        memset(&clients_[client], 0, sizeof(Client));
        strcpy(clients_[client].cid, req.cid);
      }
      Client& state = clients_[client];
      state.age = ++age_;
      if (req.seq == state.seq && !strcmp(mid, state.mid) &&
          !strcmp(req.mac, state.requestMac)) {
        if (!readCache(client, out)) {
          // Do not suggest a new sequence: a transport retry must never
          // repeat a mutation whose original response was evicted.
          const char* expired = "{\"error\":\"duplicate_expired\"}";
          respond(mid, req.cid, req.seq, 409, "application/json", expired,
                  strlen(expired), false, key, out);
        }
        return true;
      }
      if (req.seq <= state.seq) {
        snprintf(body, sizeof(body),
                 "{\"error\":\"stale_seq\",\"last_seq\":%llu}",
                 static_cast<unsigned long long>(state.seq));
        respond(mid, req.cid, req.seq, 409, "application/json", body,
                strlen(body), false, key, out);
        return true;
      }
      consume = true;
      state.seq = req.seq;
      strcpy(state.mid, mid);
      strcpy(state.requestMac, req.mac);
      cacheClient_ = client;
      gm2StreamXor(encKey_, id_, nonce_, req.cid, req.seq, mid, "req", req.iv,
                   plaintext_, static_cast<size_t>(plainLen));
      plaintext_[plainLen] = 0;
      if (memchr(plaintext_, 0, static_cast<size_t>(plainLen))) {
        status = 400;
        error = "bad_request";
      }
      // Only the streaming endpoint is unsupported; chunked uploads
      // use ordinary bounded form requests.
      if (error) {
        // Authenticated malformed plaintext consumes its sequence.
      } else if (!strcmp(req.method, "POST") && !strcmp(req.path, "/api/ota")) {
        status = 501;
        error = "not_supported";
      } else if (busy_) {
        status = 503;
        error = "busy";
      } else {
        PortalRequest request = {req.method, req.path, key, plaintext_,
                                 static_cast<size_t>(plainLen)};
        PortalReply reply = {};
        if (portal_.handle(request, &reply))
          respond(mid, req.cid, req.seq, reply.status, reply.contentType,
                  reply.body, reply.len, reply.restartAfter, key, out);
        else {
          status = 404;
          error = "not_found";
        }
      }
    }
  }
  if (error) {
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);
    respond(valid ? mid : "", valid ? req.cid : "", valid ? req.seq : 0, status,
            "application/json", body, strlen(body), false,
            signedReply ? key : nullptr, out);
  }
  if (consume) {
    storeCache(cacheClient_, *out);
  }
  return true;
}
}  // namespace gm
