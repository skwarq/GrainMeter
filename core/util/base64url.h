// SPDX-License-Identifier: MIT
#ifndef GM_BASE64URL_H
#define GM_BASE64URL_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// Returns the decoded byte count, or -1 for invalid input / insufficient
// capacity. Accepts either alphabet and optional trailing padding.
// out may point to the start of input (in-place decoding).
int base64urlDecode(const char* input, size_t len, uint8_t* out, size_t cap);

}  // namespace gm
#endif
