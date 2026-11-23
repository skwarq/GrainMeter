// SPDX-License-Identifier: MIT
#pragma once

#include "app/config.h"
#include "nvs.h"

// Configuration and ROM map in NVS namespace "grainmeter".
class NvsStore : public gm::KeyValueStore {
 public:
  esp_err_t open();
  bool get(const char* key, char* out, size_t cap) override;
  bool set(const char* key, const char* value) override;
  bool commit() override;
  bool isOpen() const { return handle_ != 0; }

 private:
  nvs_handle_t handle_ = 0;
};
