// SPDX-License-Identifier: MIT
#ifndef GM_HTTP_DATE_H
#define GM_HTTP_DATE_H

#include <stdint.h>

namespace gm {

// Parses an HTTP Date header (RFC 7231 IMF-fixdate,
// "Wed, 26 Nov 2026 10:17:05 GMT") into UTC epoch seconds.
bool parseHttpDate(const char* s, uint32_t* epoch);

}  // namespace gm

#endif
