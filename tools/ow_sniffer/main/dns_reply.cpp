// SPDX-License-Identifier: MIT
#include "dns_reply.h"

#include <cstring>

namespace ow {
int dnsBuildReply(const uint8_t* req, int len, uint32_t ip, uint8_t* out,
                  int cap) {
  if (len < 12 || len > cap - 16) return 0;
  const uint16_t flags = static_cast<uint16_t>((req[2] << 8) | req[3]);
  const uint16_t questions = static_cast<uint16_t>((req[4] << 8) | req[5]);
  if ((flags & 0x8000) || ((flags >> 11) & 15) || questions != 1) return 0;
  int pos = 12;
  while (true) {
    if (pos >= len) return 0;
    const uint8_t label = req[pos++];
    if (!label) break;
    if (label > 63 || pos + label > len) return 0;
    pos += label;
  }
  if (pos + 4 > len) return 0;
  const uint16_t type = static_cast<uint16_t>((req[pos] << 8) | req[pos + 1]);
  const uint16_t cls =
      static_cast<uint16_t>((req[pos + 2] << 8) | req[pos + 3]);
  const int end = pos + 4;
  std::memcpy(out, req, static_cast<size_t>(end));
  out[2] = 0x81 | (req[2] & 1);
  out[3] = 0x80;
  out[6] = 0;
  out[7] = type == 1 && cls == 1 ? 1 : 0;
  out[8] = out[9] = out[10] = out[11] = 0;
  int n = end;
  if (out[7]) {
    const uint8_t answer[16] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0,
                                0,    0,    0, 4, 0, 0, 0, 0};
    std::memcpy(out + n, answer, sizeof(answer));
    std::memcpy(out + n + 12, &ip, 4);
    n += sizeof(answer);
  }
  return n;
}
}  // namespace ow
