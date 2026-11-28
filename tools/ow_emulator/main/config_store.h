// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include "ow_slave.h"

constexpr size_t kMaxConfigJson = 12288;
bool configParse(const char* json, ow::Config* config, char* error,
                 size_t errorSize);
char* configJson(const ow::Config& config);  // release using cJSON_free
esp_err_t configLoad(ow::Config* config);
esp_err_t configSave(const ow::Config& config);
