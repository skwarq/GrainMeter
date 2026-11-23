// SPDX-License-Identifier: MIT
#ifndef GM_JSON_READ_H
#define GM_JSON_READ_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// Minimal reader for top-level members of a small, flat JSON object (the
// remote update manifest). Nested objects/arrays are skipped. Strings
// support the standard escapes except \u (rejected). Returns false when the
// key is missing, has another type, or the document is malformed.
bool jsonGetString(const char* json, const char* key, char* out, size_t cap);
bool jsonGetUint(const char* json, const char* key, uint32_t* out);

}  // namespace gm

#endif
