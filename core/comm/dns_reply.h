// SPDX-License-Identifier: MIT
#ifndef GM_DNS_REPLY_H
#define GM_DNS_REPLY_H

#include <stdint.h>

namespace gm {

// Captive-portal DNS answer: every A query is answered with the SoftAP
// address, with strict bounds checks. ipv4 is in network byte order. Returns
// the reply length, or 0 when the packet must be ignored (malformed, not a
// standard query).
int dnsBuildReply(const uint8_t* req, int len, uint32_t ipv4, uint8_t* out,
                  int cap);

}  // namespace gm

#endif
