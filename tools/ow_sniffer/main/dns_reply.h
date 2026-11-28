// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace ow {
// Local copy of core/comm/dns_reply: IP is in network byte order.
int dnsBuildReply(const uint8_t* req, int len, uint32_t ip, uint8_t* out,
                  int cap);
}  // namespace ow
