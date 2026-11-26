// SPDX-License-Identifier: MIT
#ifndef GM_TEST_OTA_UPLOAD_H
#define GM_TEST_OTA_UPLOAD_H

#include <string>
#include <vector>

#include "util/sha256.h"

inline std::string imageHash(const std::vector<uint8_t>& image) {
  gm::Sha256 sha;
  uint8_t digest[32];
  char hex[65];
  sha.update(image.data(), image.size());
  sha.finish(digest);
  gm::sha256ToHex(digest, hex);
  return hex;
}

inline std::string encodeChunk(const uint8_t* data, size_t len) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  unsigned bits = 0, count = 0;
  for (size_t i = 0; i < len; ++i) {
    bits = (bits << 8) | data[i];
    count += 8;
    while (count >= 6) {
      count -= 6;
      out.push_back(alphabet[(bits >> count) & 63]);
    }
  }
  if (count) out.push_back(alphabet[(bits << (6 - count)) & 63]);
  return out;
}
#endif
