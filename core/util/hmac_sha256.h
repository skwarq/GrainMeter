// SPDX-License-Identifier: MIT
#ifndef GM_HMAC_SHA256_H
#define GM_HMAC_SHA256_H
#include "util/sha256.h"
namespace gm {
class HmacSha256 {
 public:
  HmacSha256(const void* key, size_t len);
  void update(const void* data, size_t len) { inner_.update(data, len); }
  void finish(uint8_t digest[32]);

 private:
  Sha256 inner_;
  uint8_t outer_[64];
};
void hmacSha256Hex(const void* key, size_t keyLen, const void* data, size_t len,
                   char out[65]);
bool constantTimeEqual(const void* a, const void* b, size_t len);
bool pbkdf2HmacSha256(const void* password, size_t passwordLen,
                      const void* salt, size_t saltLen, uint32_t iterations,
                      uint8_t* out, size_t outLen);
void gm2DeriveKeys(const char* password, const char* id, uint8_t mac[32],
                   uint8_t enc[32]);
// In-place XOR; offset permits bounded streaming chunks of the same message.
void gm2StreamXor(const uint8_t key[32], const char* id, const char* nonce,
                  const char* cid, uint64_t seq, const char* mid,
                  const char* direction, const char* iv, void* data, size_t len,
                  size_t offset = 0);
}  // namespace gm
#endif
