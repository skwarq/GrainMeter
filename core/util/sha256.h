// SPDX-License-Identifier: MIT
#ifndef GM_SHA256_H
#define GM_SHA256_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// Streaming SHA-256 (FIPS 180-4). Used to verify remotely downloaded
// firmware against the hash published in the update manifest.
class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const void* data, size_t len);
  void finish(uint8_t digest[32]);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t bufLen_;
  uint64_t total_;
};

// Parses 64 hex characters.
bool sha256FromHex(const char* hex, uint8_t out[32]);
void sha256ToHex(const uint8_t digest[32], char out[65]);

}  // namespace gm

#endif
