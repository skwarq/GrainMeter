// SPDX-License-Identifier: MIT
#include "platform/nvs_store.h"

#include <cstring>

#include "esp_log.h"

static const char* kTag = "nvs";

esp_err_t NvsStore::open() {
  return nvs_open("grainmeter", NVS_READWRITE, &handle_);
}

bool NvsStore::get(const char* key, char* out, size_t cap) {
  if (handle_ == 0) {
    return false;
  }
  size_t len = cap;
  const esp_err_t err = nvs_get_str(handle_, key, out, &len);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(kTag, "read %s: %s", key, esp_err_to_name(err));
  }
  return err == ESP_OK;
}

bool NvsStore::set(const char* key, const char* value) {
  if (handle_ == 0) {
    return false;
  }
  // Skip unchanged values to save flash wear.
  char current[256];
  size_t len = sizeof(current);
  if (nvs_get_str(handle_, key, current, &len) == ESP_OK &&
      std::strcmp(current, value) == 0) {
    return true;
  }
  const esp_err_t err = nvs_set_str(handle_, key, value);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "write %s: %s", key,
             esp_err_to_name(err));  // key name only, never the value
    return false;
  }
  return true;
}

bool NvsStore::commit() {
  if (handle_ == 0) {
    return false;
  }
  const esp_err_t err = nvs_commit(handle_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "commit: %s", esp_err_to_name(err));
  }
  return err == ESP_OK;
}
