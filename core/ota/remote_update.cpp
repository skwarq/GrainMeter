// SPDX-License-Identifier: MIT
#include "ota/remote_update.h"

#include <string.h>

#include "util/json_read.h"
#include "util/sha256.h"

namespace gm {

bool parseUpdateManifest(const char* json, UpdateManifest* m,
                         const char** err) {
  memset(m, 0, sizeof(*m));
  *err = nullptr;
  char hex[80];
  const bool hasVersion =
      jsonGetString(json, "version", m->version, sizeof(m->version));
  const bool hasUrl = jsonGetString(json, "url", m->url, sizeof(m->url));
  const bool hasSha = jsonGetString(json, "sha256", hex, sizeof(hex));
  if (hasVersion || hasUrl || hasSha) {
    if (!hasVersion || !hasUrl || !hasSha) {
      *err = "manifest: version, url and sha256 are required together";
      return false;
    }
    if (!sha256FromHex(hex, m->sha256)) {
      *err = "manifest: sha256 must be 64 hex characters";
      return false;
    }
    if (m->version[0] < '0' || m->version[0] > '9') {
      *err = "manifest: version must start with a digit";
      return false;
    }
    m->hasFirmware = true;
  }
  uint32_t v = 0;
  if (jsonGetUint(json, "service_id", &v)) m->serviceId = v;
  if (jsonGetUint(json, "service_min", &v)) m->serviceMin = v;
  if (m->serviceId != 0 && (m->serviceMin < 1 || m->serviceMin > 60)) {
    *err = "manifest: service_min must be 1..60";
    return false;
  }
  if (!m->hasFirmware && m->serviceId == 0) {
    *err = "manifest: no firmware and no service request";
    return false;
  }
  return true;
}

static const char* readNum(const char* p, unsigned long* out) {
  unsigned long n = 0;
  while (*p >= '0' && *p <= '9') {
    n = n * 10u + static_cast<unsigned long>(*p - '0');
    ++p;
  }
  *out = n;
  return p;
}

int compareVersions(const char* a, const char* b) {
  while (true) {
    unsigned long na = 0, nb = 0;
    a = readNum(a, &na);
    b = readNum(b, &nb);
    if (na != nb) return na < nb ? -1 : 1;
    const bool moreA = *a == '.';
    const bool moreB = *b == '.';
    if (moreA && moreB) {
      ++a;
      ++b;
      continue;
    }
    if (moreA != moreB) {
      // "1.2" vs "1.2.0": compare remaining parts against zero
      const char* rest = moreA ? a + 1 : b + 1;
      unsigned long r = 0;
      readNum(rest, &r);
      if (r != 0) return moreA ? 1 : -1;
      if (moreA)
        ++a;
      else
        ++b;
      continue;
    }
    // end of numeric part: pre-release suffix sorts lower
    const bool preA = *a == '-';
    const bool preB = *b == '-';
    if (preA && !preB) return -1;
    if (!preA && preB) return 1;
    if (preA && preB) {
      const int c = strcmp(a, b);
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    return 0;
  }
}

}  // namespace gm
