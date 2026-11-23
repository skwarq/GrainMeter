// SPDX-License-Identifier: MIT
#ifndef GM_REMOTE_UPDATE_H
#define GM_REMOTE_UPDATE_H

#include <stdint.h>

namespace gm {

// Update manifest fetched over HTTPS during an upload window, e.g.
//   {"version":"0.2.0",
//    "url":"https://example.com/grainmeter-0.2.0-esp32c6-ota.bin",
//    "sha256":"<64 hex chars of the .bin file>",
//    "service_id":7, "service_min":10}
// version/url/sha256 describe the newest firmware (all three required to
// update). service_id/service_min (optional) ask the device to open the
// service portal for N minutes once per new service_id.
struct UpdateManifest {
  char version[33];
  char url[192];
  uint8_t sha256[32];
  bool hasFirmware;
  uint32_t serviceId;
  uint32_t serviceMin;
};

bool parseUpdateManifest(const char* json, UpdateManifest* m, const char** err);

// Compares dotted numeric versions ("1.10.2" > "1.9.7"). A pre-release
// suffix ("-rc1", "-dev") sorts before the plain version. Returns <0, 0, >0.
int compareVersions(const char* a, const char* b);

}  // namespace gm

#endif
