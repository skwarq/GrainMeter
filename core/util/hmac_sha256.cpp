// SPDX-License-Identifier: MIT
#include "util/hmac_sha256.h"

#include <stdio.h>
#include <string.h>
namespace gm {
HmacSha256::HmacSha256(const void* key, size_t len) {
  uint8_t block[64] = {};
  if (len > sizeof(block)) {
    Sha256 hash;
    hash.update(key, len);
    hash.finish(block);
  } else if (len) {
    memcpy(block, key, len);
  }
  for (size_t i = 0; i < sizeof(block); ++i) {
    outer_[i] = block[i] ^ 0x5c;
    block[i] ^= 0x36;
  }
  inner_.update(block, sizeof(block));
}
void HmacSha256::finish(uint8_t digest[32]) {
  uint8_t inner[32];
  inner_.finish(inner);
  Sha256 hash;
  hash.update(outer_, sizeof(outer_));
  hash.update(inner, sizeof(inner));
  hash.finish(digest);
}
void hmacSha256Hex(const void* key, size_t keyLen, const void* data, size_t len,
                   char out[65]) {
  HmacSha256 hash(key, keyLen);
  hash.update(data, len);
  uint8_t digest[32];
  hash.finish(digest);
  sha256ToHex(digest, out);
}
bool constantTimeEqual(const void* a, const void* b, size_t len) {
  const uint8_t* x = static_cast<const uint8_t*>(a);
  const uint8_t* y = static_cast<const uint8_t*>(b);
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i)
    diff = static_cast<uint8_t>(diff | (x[i] ^ y[i]));
  return diff == 0;
}
bool pbkdf2HmacSha256(const void* password, size_t passwordLen,
                      const void* salt, size_t saltLen, uint32_t iterations,
                      uint8_t* out, size_t outLen) {
  if (!iterations || (outLen && !out) || (passwordLen && !password) ||
      (saltLen && !salt) || outLen / 32 > UINT32_MAX - 1)
    return false;
  uint32_t block = 1;
  const HmacSha256 initialized(password, passwordLen);
  while (outLen) {
    uint8_t u[32], result[32];
    const uint8_t index[] = {
        static_cast<uint8_t>(block >> 24), static_cast<uint8_t>(block >> 16),
        static_cast<uint8_t>(block >> 8), static_cast<uint8_t>(block)};
    HmacSha256 first = initialized;
    first.update(salt, saltLen);
    first.update(index, 4);
    first.finish(u);
    memcpy(result, u, 32);
    for (uint32_t i = 1; i < iterations; ++i) {
      HmacSha256 h = initialized;
      h.update(u, 32);
      h.finish(u);
      for (size_t j = 0; j < 32; ++j) result[j] ^= u[j];
    }
    const size_t n = outLen < 32 ? outLen : 32;
    memcpy(out, result, n);
    out += n;
    outLen -= n;
    ++block;
  }
  return true;
}
void gm2DeriveKeys(const char* password, const char* id, uint8_t mac[32],
                   uint8_t enc[32]) {
  char salt[64];
  snprintf(salt, sizeof(salt), "gm2|%s", id);
  uint8_t key[32];
  pbkdf2HmacSha256(password, strlen(password), salt, strlen(salt), 10000, key,
                   sizeof(key));
  HmacSha256 m(key, 32), e(key, 32);
  m.update("mac", 3);
  m.finish(mac);
  e.update("enc", 3);
  e.finish(enc);
}
void gm2StreamXor(const uint8_t key[32], const char* id, const char* nonce,
                  const char* cid, uint64_t seq, const char* mid,
                  const char* direction, const char* iv, void* data, size_t len,
                  size_t offset) {
  uint8_t* bytes = static_cast<uint8_t*>(data);
  char number[24], block[24];
  snprintf(number, sizeof(number), "%llu",
           static_cast<unsigned long long>(seq));
  while (len) {
    snprintf(block, sizeof(block), "%llu",
             static_cast<unsigned long long>(offset / 32));
    const char* parts[] = {"gm2stream", id,        nonce, cid,  number,
                           mid,         direction, iv,    block};
    HmacSha256 h(key, 32);
    for (size_t i = 0; i < 9; ++i) {
      if (i) h.update("\n", 1);
      h.update(parts[i], strlen(parts[i]));
    }
    uint8_t stream[32];
    h.finish(stream);
    size_t n = 32 - offset % 32;
    if (n > len) n = len;
    for (size_t i = 0; i < n; ++i) bytes[i] ^= stream[offset % 32 + i];
    bytes += n;
    len -= n;
    offset += n;
  }
}
}  // namespace gm
