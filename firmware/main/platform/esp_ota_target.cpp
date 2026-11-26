// SPDX-License-Identifier: MIT
#include "platform/esp_ota_target.h"

#include <cstdio>

#include "esp_log.h"
#include "sdkconfig.h"

static const char* kTag = "ota";

size_t EspOtaTarget::slotSize() {
  const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
  return p != nullptr ? p->size : 0;
}

const char* EspOtaTarget::slotName() {
  const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
  return p != nullptr ? p->label : "none";
}

bool EspOtaTarget::begin(size_t imageSize, char* err, size_t cap) {
  part_ = esp_ota_get_next_update_partition(nullptr);
  if (part_ == nullptr) {
    snprintf(err, cap, "no OTA slot available");
    return false;
  }
  // imageSize may be an upper bound (multipart): only that range is erased.
  const esp_err_t e = esp_ota_begin(part_, imageSize, &handle_);
  if (e != ESP_OK) {
    snprintf(err, cap, "esp_ota_begin: %s", esp_err_to_name(e));
    handle_ = 0;
    return false;
  }
  ESP_LOGI(kTag, "writing to %s at 0x%lx", part_->label,
           static_cast<unsigned long>(part_->address));
  return true;
}

bool EspOtaTarget::write(const uint8_t* data, size_t len, char* err,
                         size_t cap) {
  const esp_err_t e = esp_ota_write(handle_, data, len);
  if (e != ESP_OK) {
    snprintf(err, cap, "esp_ota_write: %s", esp_err_to_name(e));
    return false;
  }
  return true;
}

bool EspOtaTarget::finish(char* err, size_t cap) {
  esp_err_t e = esp_ota_end(handle_);  // validates the whole image
  handle_ = 0;
  if (e != ESP_OK) {
#if CONFIG_SECURE_SIGNED_ON_UPDATE
    // Signed profile: esp_ota_end() also checks the RSA signature.
    static const char kInvalid[] =
        "image rejected: not signed with the project key, or corrupt";
#else
    static const char kInvalid[] =
        "image verification failed (corrupt or truncated)";
#endif
    snprintf(err, cap, "%s",
             e == ESP_ERR_OTA_VALIDATE_FAILED ? kInvalid : esp_err_to_name(e));
    return false;
  }
  e = esp_ota_set_boot_partition(part_);
  if (e != ESP_OK) {
    snprintf(err, cap, "set boot partition: %s", esp_err_to_name(e));
    return false;
  }
  ESP_LOGI(kTag, "%s selected for next boot (pending verification)",
           part_->label);
  return true;
}

void EspOtaTarget::abort() {
  if (handle_ != 0) {
    esp_ota_abort(handle_);
    handle_ = 0;
  }
}
