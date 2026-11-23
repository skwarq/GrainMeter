// SPDX-License-Identifier: MIT
#pragma once

#include "esp_ota_ops.h"
#include "ota/ota_session.h"

// Native ESP-IDF OTA: streams into the inactive slot; esp_ota_end() verifies
// the complete image (format, chip, SHA-256) before the boot partition is
// switched. An aborted transfer leaves the current boot partition selected.
class EspOtaTarget : public gm::OtaTarget {
 public:
  size_t slotSize() override;
  const char* slotName() override;
  bool begin(size_t imageSize, char* err, size_t cap) override;
  bool write(const uint8_t* data, size_t len, char* err, size_t cap) override;
  bool finish(char* err, size_t cap) override;
  void abort() override;

 private:
  const esp_partition_t* part_ = nullptr;
  esp_ota_handle_t handle_ = 0;
};
