// SPDX-License-Identifier: MIT
#ifndef GM_OTA_IMAGE_H
#define GM_OTA_IMAGE_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// Layout of an ESP-IDF application image (esp_app_format.h):
//   0   esp_image_header_t (24 B): magic 0xE9, segment_count, ..., chip_id @12
//   24  first esp_image_segment_header_t (8 B)
//   32  esp_app_desc_t: magic 0xABCD5432, secure_version, reserved[2],
//       version[32] @48, project_name[32] @80, time[16], date[16],
//       idf_ver[32] @144, ...
enum {
  kImageMagic = 0xE9,
  kAppDescMagic = 0xABCD5432u,
  kChipIdEsp32C6 = 0x000D,
  kImageProbeLen = 32 + 256,  // headers needed for validation
  kMinImageLen = 4096,
};

struct ImageInfo {
  uint16_t chipId;
  uint8_t segments;
  char version[33];
  char project[33];
  char idfVersion[33];
};

// Checks the first kImageProbeLen bytes of an upload. expectedProject may be
// NULL to skip the project-name check. Returns false with err set.
bool otaInspectImage(const uint8_t* data, size_t len, uint16_t expectedChip,
                     const char* expectedProject, ImageInfo* info,
                     const char** err);

}  // namespace gm

#endif
