// SPDX-License-Identifier: MIT
#ifndef GM_TEST_GM2_H
#define GM_TEST_GM2_H
#include <array>
#include <map>
#include <string>
#include <vector>

#include "util/base64url.h"
#include "util/hmac_sha256.h"
#include "util/json.h"
#include "util/json_read.h"
namespace gm2test {
constexpr const char* cid = "0123456789abcdef";
constexpr const char* iv = "abcdef0123456789abcdef0123456789";
struct Keys {
  std::array<uint8_t, 32> mac, enc;
};
inline const Keys& keys(const char* password, const char* id) {
  static std::map<std::string, Keys> cache;
  const std::string name = std::string(password) + "\n" + id;
  auto found = cache.find(name);
  if (found == cache.end()) {
    Keys k{};
    gm::gm2DeriveKeys(password, id, k.mac.data(), k.enc.data());
    found = cache.emplace(name, k).first;
  }
  return found->second;
}
inline std::string hmac(const uint8_t key[32], const std::string& text) {
  char out[65];
  gm::hmacSha256Hex(key, 32, text.data(), text.size(), out);
  return out;
}
inline std::string quote(const std::string& text) {
  std::vector<char> buf(text.size() * 6 + 3);
  gm::JsonWriter w(buf.data(), buf.size());
  w.str(text.c_str());
  return buf.data();
}
inline std::string base64(const std::string& bytes) {
  static const char* abc =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint8_t a = static_cast<uint8_t>(bytes[i]),
                  b = i + 1 < bytes.size() ? static_cast<uint8_t>(bytes[i + 1])
                                           : 0,
                  c = i + 2 < bytes.size() ? static_cast<uint8_t>(bytes[i + 2])
                                           : 0;
    out += abc[a >> 2];
    out += abc[((a & 3) << 4) | (b >> 4)];
    out += i + 1 < bytes.size() ? abc[((b & 15) << 2) | (c >> 6)] : '=';
    out += i + 2 < bytes.size() ? abc[c & 63] : '=';
  }
  return out;
}
inline std::string field(const char* json, const char* key) {
  std::vector<char> out(22000);
  if (!gm::jsonGetString(json, key, out.data(), out.size())) return "";
  return out.data();
}
inline std::string request(const char* id, const char* nonce, uint64_t seq,
                           const char* method, const char* path,
                           const std::string& plaintext, const char* mid,
                           const char* password = "test-password",
                           const char* client = cid,
                           const char* requestIv = iv) {
  const Keys& k = keys(password, id);
  std::string bytes = plaintext;
  gm::gm2StreamXor(k.enc.data(), id, nonce, client, seq, mid, "req", requestIv,
                   bytes.empty() ? nullptr : &bytes[0], bytes.size());
  const std::string body = base64(bytes), num = std::to_string(seq);
  const std::string text = std::string("gm2req\n") + id + "\n" + nonce + "\n" +
                           client + "\n" + num + "\n" + mid + "\n" + method +
                           "\n" + path + "\n" + requestIv + "\n" + body;
  return "{\"v\":2,\"cid\":" + quote(client) + ",\"seq\":" + num +
         ",\"method\":" + quote(method) + ",\"path\":" + quote(path) +
         ",\"iv\":" + quote(requestIv) + ",\"body\":" + quote(body) +
         ",\"mac\":" + quote(hmac(k.mac.data(), text)) + "}";
}
inline std::string responseText(const char* json, const char* id,
                                const char* nonce, uint64_t seq,
                                const char* mid) {
  uint32_t status = 0;
  gm::jsonGetUint(json, "status", &status);
  const bool restart =
      std::string(json).find("\"restart\":true") != std::string::npos;
  return std::string("gm2res\n") + id + "\n" + nonce + "\n" +
         field(json, "cid") + "\n" + std::to_string(seq) + "\n" + mid + "\n" +
         std::to_string(status) + "\n" + field(json, "ctype") + "\n" +
         field(json, "enc") + "\n" + (restart ? "1" : "0") + "\n" +
         field(json, "iv") + "\n" + field(json, "body");
}
inline bool validMac(const char* json, const char* id, const char* nonce,
                     uint64_t seq, const char* mid,
                     const char* password = "test-password") {
  return field(json, "mac") == hmac(keys(password, id).mac.data(),
                                    responseText(json, id, nonce, seq, mid));
}
inline std::string plaintext(const char* json, const char* id,
                             const char* nonce, uint64_t seq, const char* mid,
                             const char* password = "test-password") {
  const std::string body = field(json, "body");
  if (field(json, "mac").empty()) return body;
  std::vector<uint8_t> decoded(body.size());
  const int len = gm::base64urlDecode(body.data(), body.size(), decoded.data(),
                                      decoded.size());
  if (len < 0) return "";
  gm::gm2StreamXor(keys(password, id).enc.data(), id, nonce,
                   field(json, "cid").c_str(), seq, mid, "res",
                   field(json, "iv").c_str(), decoded.data(),
                   static_cast<size_t>(len));
  return std::string(decoded.begin(), decoded.begin() + len);
}
inline std::string statusText(const char* json, const char* id) {
  std::vector<char> buf(4096);
  uint32_t until = 0, remaining = 0, budget = 0, iterations = 0;
  gm::jsonGetUint(json, "until", &until);
  gm::jsonGetUint(json, "remaining_s", &remaining);
  gm::jsonGetUint(json, "budget_left_s", &budget);
  gm::jsonGetUint(json, "kdf_iter", &iterations);
  gm::JsonWriter w(buf.data(), buf.size());
  w.beginArray()
      .unum(2)
      .str(id)
      .str("online")
      .str(field(json, "nonce").c_str())
      .str(field(json, "fw").c_str())
      .str(field(json, "name").c_str())
      .unum(until)
      .unum(remaining)
      .unum(budget)
      .unum(iterations);
  const std::string source(json);
  const size_t pos = source.find("\"op\":");
  const size_t obj = pos == std::string::npos
                         ? pos
                         : source.find_first_not_of(" \t\r\n", pos + 5);
  if (obj != std::string::npos && source[obj] == '{') {
    const char* op = json + obj;
    w.beginArray()
        .str(field(op, "mid").c_str())
        .str(field(op, "fw").c_str())
        .str(field(op, "result").c_str())
        .endArray();
  } else
    w.null();
  w.endArray();
  return std::string("gm2status\n") + buf.data();
}
inline bool validStatus(const char* json, const char* id,
                        const char* password = "test-password") {
  return field(json, "mac") ==
         hmac(keys(password, id).mac.data(), statusText(json, id));
}
}  // namespace gm2test
#endif
