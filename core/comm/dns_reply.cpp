// SPDX-License-Identifier: MIT
#include "comm/dns_reply.h"

#include <string.h>

namespace gm {

int dnsBuildReply(const uint8_t* req, int len, uint32_t ip, uint8_t* out,
                  int cap) {
  if (len < 12 || len > cap - 16) {
    return 0;
  }
  const uint16_t flags = static_cast<uint16_t>((req[2] << 8) | req[3]);
  const uint16_t qdcount = static_cast<uint16_t>((req[4] << 8) | req[5]);
  if ((flags & 0x8000) != 0 || ((flags >> 11) & 0x0F) != 0 || qdcount != 1) {
    return 0;  // not a standard query with one question
  }
  // Walk the QNAME labels without leaving the packet.
  int pos = 12;
  while (true) {
    if (pos >= len) return 0;
    const uint8_t l = req[pos];
    if (l == 0) {
      ++pos;
      break;
    }
    if ((l & 0xC0) != 0 || pos + 1 + l > len)
      return 0;  // no compression in questions
    pos += 1 + l;
  }
  if (pos + 4 > len) return 0;
  const uint16_t qtype = static_cast<uint16_t>((req[pos] << 8) | req[pos + 1]);
  const int qend = pos + 4;

  memcpy(out, req, static_cast<size_t>(qend));
  out[2] = 0x81 | (req[2] & 0x01);  // QR=1, opcode 0, keep RD
  out[3] = 0x80;                    // RA=1, RCODE=0
  out[6] = 0;
  out[7] = qtype == 1 ? 1 : 0;  // one answer for A, none otherwise
  out[8] = out[9] = out[10] = out[11] = 0;
  int n = qend;
  if (qtype == 1) {
    const uint8_t answer[16] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
                                0x00, 0x3C, 0x00, 0x04, 0,    0,    0,    0};
    memcpy(out + n, answer, sizeof(answer));
    memcpy(out + n + 12, &ip, 4);  // already network order
    n += 16;
  }
  return n;
}

}  // namespace gm
