// SPDX-License-Identifier: MIT
#include "util/crc.h"

namespace gm {

uint8_t crc8Maxim(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    uint8_t in = data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint8_t mix = static_cast<uint8_t>((crc ^ in) & 0x01u);
      crc = static_cast<uint8_t>(crc >> 1);
      if (mix) {
        crc = static_cast<uint8_t>(crc ^ 0x8Cu);
      }
      in = static_cast<uint8_t>(in >> 1);
    }
  }
  return crc;
}

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = ~seed;
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

}  // namespace gm
