// SPDX-License-Identifier: MIT
#include "ota/ota_image.h"

#include <string.h>

namespace gm {

static uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static void copyField(char* dst, const uint8_t* src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i] != 0; ++i) {
    const char c = static_cast<char>(src[i]);
    dst[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
  }
  dst[i] = '\0';
}

bool otaInspectImage(const uint8_t* data, size_t len, uint16_t expectedChip,
                     const char* expectedProject, ImageInfo* info,
                     const char** err) {
  if (len < kImageProbeLen) {
    *err = "file too short for an ESP-IDF application image";
    return false;
  }
  if (data[0] != kImageMagic) {
    *err = "not an ESP-IDF application image (bad magic byte)";
    return false;
  }
  info->segments = data[1];
  if (info->segments == 0 || info->segments > 16) {
    *err = "invalid segment count";
    return false;
  }
  info->chipId = static_cast<uint16_t>(data[12] | (data[13] << 8));
  if (info->chipId != expectedChip) {
    *err = "image built for a different chip";
    return false;
  }
  if (le32(data + 32) != kAppDescMagic) {
    *err = "application descriptor missing (bootloader or partition image?)";
    return false;
  }
  copyField(info->version, data + 48, 32);
  copyField(info->project, data + 80, 32);
  copyField(info->idfVersion, data + 144, 32);
  if (expectedProject != nullptr &&
      strcmp(info->project, expectedProject) != 0) {
    *err = "image is for a different project";
    return false;
  }
  *err = nullptr;
  return true;
}

}  // namespace gm
