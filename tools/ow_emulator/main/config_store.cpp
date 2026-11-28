// SPDX-License-Identifier: MIT
#include "config_store.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

namespace {
const char* kTag = "config";

bool fail(char* error, size_t size, const char* message) {
  std::snprintf(error, size, "%s", message);
  return false;
}

int hexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool hexBytes(const char* text, uint8_t* bytes, size_t min, size_t max,
              size_t* count) {
  size_t digits = 0;
  for (const char* p = text; *p; ++p) {
    if (*p == ' ' || *p == ':' || *p == '-') continue;
    const int digit = hexDigit(*p);
    if (digit < 0 || digits >= max * 2) return false;
    if ((digits & 1) == 0)
      bytes[digits / 2] = static_cast<uint8_t>(digit << 4);
    else
      bytes[digits / 2] |= digit;
    ++digits;
  }
  *count = digits / 2;
  return !(digits & 1) && *count >= min;
}

void toHex(const uint8_t* bytes, size_t count, char* text) {
  for (size_t i = 0; i < count; ++i)
    std::snprintf(text + i * 2, 3, "%02X", bytes[i]);
}

bool number(const cJSON* obj, const char* key, double min, double max,
            double* value, bool integral = true) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!item) return true;
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
      item->valuedouble < min || item->valuedouble > max ||
      (integral && std::floor(item->valuedouble) != item->valuedouble))
    return false;
  *value = item->valuedouble;
  return true;
}

bool boolean(const cJSON* obj, const char* key, bool* value) {
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!item) return true;
  if (!cJSON_IsBool(item)) return false;
  *value = cJSON_IsTrue(item);
  return true;
}

bool parseDevice(const cJSON* obj, ow::DeviceConfig* cfg, char* error,
                 size_t errorSize) {
  if (!cJSON_IsObject(obj))
    return fail(error, errorSize, "device must be object");
  const cJSON* rom = cJSON_GetObjectItemCaseSensitive(obj, "rom");
  size_t count = 0;
  if (!cJSON_IsString(rom) ||
      !hexBytes(rom->valuestring, cfg->rom, 7, 8, &count))
    return fail(error, errorSize, "ROM must contain 7 or 8 hex bytes");
  if (count == 7) cfg->rom[7] = ow::crc8(cfg->rom, 7);
  if (!boolean(obj, "enabled", &cfg->enabled) ||
      !boolean(obj, "powered", &cfg->powered) ||
      !boolean(obj, "power_on_85", &cfg->powerOn85))
    return fail(error, errorSize,
                "enabled/powered/power_on_85 must be boolean");
  const cJSON* source = cJSON_GetObjectItemCaseSensitive(obj, "source");
  if (source) {
    if (!cJSON_IsString(source) ||
        (std::strcmp(source->valuestring, "fixed") != 0 &&
         std::strcmp(source->valuestring, "real") != 0))
      return fail(error, errorSize, "source must be fixed or real");
    cfg->realSource = std::strcmp(source->valuestring, "real") == 0;
#ifndef CONFIG_OW_REAL_SENSOR
    if (cfg->realSource)
      return fail(error, errorSize, "real sensor disabled in Kconfig");
#endif
  }
  double value = cfg->temperature / 16.0;
  if (!number(obj, "temperature", -2048, 2047.9375, &value, false))
    return fail(error, errorSize, "temperature outside int16 / 16 range");
  cfg->temperature = static_cast<int16_t>(std::lround(value * 16));
  value = cfg->conversionMs;
  if (!number(obj, "conversion_ms", 0, 60000, &value))
    return fail(error, errorSize, "conversion_ms must be integer 0..60000");
  cfg->conversionMs = static_cast<uint32_t>(value);
  value = cfg->th;
  if (!number(obj, "th", 0, 255, &value))
    return fail(error, errorSize, "th must be raw byte 0..255");
  cfg->th = static_cast<uint8_t>(value);
  value = cfg->tl;
  if (!number(obj, "tl", 0, 255, &value))
    return fail(error, errorSize, "tl must be raw byte 0..255");
  cfg->tl = static_cast<uint8_t>(value);
  value = cfg->config;
  if (!number(obj, "config", 0, 255, &value))
    return fail(error, errorSize, "config must be raw byte 0..255");
  cfg->config = static_cast<uint8_t>(value);
  const cJSON* reserved = cJSON_GetObjectItemCaseSensitive(obj, "reserved");
  if (reserved &&
      (!cJSON_IsString(reserved) ||
       !hexBytes(reserved->valuestring, cfg->reserved, 3, 3, &count)))
    return fail(error, errorSize, "reserved must contain 3 hex bytes");
  const cJSON* raw = cJSON_GetObjectItemCaseSensitive(obj, "raw");
  if (raw && !cJSON_IsNull(raw)) {
    if (!cJSON_IsString(raw))
      return fail(error, errorSize, "raw must be null or 8 hex bytes");
    if (raw->valuestring[0]) {
      if (!hexBytes(raw->valuestring, cfg->raw, 8, 8, &count))
        return fail(error, errorSize, "raw must contain exactly 8 hex bytes");
      cfg->rawEnabled = true;
    }
  }
  return true;
}
}  // namespace

bool configParse(const char* json, ow::Config* config, char* error,
                 size_t errorSize) {
  cJSON* root = cJSON_ParseWithOpts(json, nullptr, true);
  if (!root) return fail(error, errorSize, "invalid JSON");
  const cJSON* devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
  const int count = cJSON_GetArraySize(devices);
  bool valid = cJSON_IsObject(root) && cJSON_IsArray(devices) && count >= 1 &&
               count <= static_cast<int>(ow::kMaxDevices);
  if (!valid) fail(error, errorSize, "devices must be array of 1..8 objects");
  ow::Config parsed;
  parsed.count = static_cast<uint8_t>(count);
  for (int i = 0; valid && i < count; ++i)
    valid = parseDevice(cJSON_GetArrayItem(devices, i), &parsed.devices[i],
                        error, errorSize);
  cJSON_Delete(root);
  if (valid) *config = parsed;
  return valid;
}

char* configJson(const ow::Config& config) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return nullptr;
  cJSON* devices = cJSON_AddArrayToObject(root, "devices");
  bool ok = devices != nullptr;
  for (size_t i = 0; ok && i < config.count; ++i) {
    const auto& cfg = config.devices[i];
    cJSON* obj = cJSON_CreateObject();
    if (!obj) {
      ok = false;
      break;
    }
    if (!cJSON_AddItemToArray(devices, obj)) {
      cJSON_Delete(obj);
      ok = false;
      break;
    }
    char rom[17] = {};
    char reserved[7] = {};
    char raw[17] = {};
    toHex(cfg.rom, 8, rom);
    toHex(cfg.reserved, 3, reserved);
    if (cfg.rawEnabled) toHex(cfg.raw, 8, raw);
    ok = cJSON_AddBoolToObject(obj, "enabled", cfg.enabled) &&
         cJSON_AddStringToObject(obj, "rom", rom) &&
         cJSON_AddBoolToObject(obj, "rom_crc_valid",
                               ow::crc8(cfg.rom, 7) == cfg.rom[7]) &&
         cJSON_AddNumberToObject(obj, "temperature", cfg.temperature / 16.0) &&
         cJSON_AddStringToObject(obj, "source",
                                 cfg.realSource ? "real" : "fixed") &&
         cJSON_AddNumberToObject(obj, "conversion_ms", cfg.conversionMs) &&
         cJSON_AddBoolToObject(obj, "powered", cfg.powered) &&
         cJSON_AddBoolToObject(obj, "power_on_85", cfg.powerOn85) &&
         cJSON_AddNumberToObject(obj, "th", cfg.th) &&
         cJSON_AddNumberToObject(obj, "tl", cfg.tl) &&
         cJSON_AddNumberToObject(obj, "config", cfg.config) &&
         cJSON_AddStringToObject(obj, "reserved", reserved) &&
         cJSON_AddStringToObject(obj, "raw", raw);
  }
  char* json = ok ? cJSON_PrintUnformatted(root) : nullptr;
  cJSON_Delete(root);
  return json;
}

esp_err_t configLoad(ow::Config* config) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("ow_emu", NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    *config = ow::defaultConfig();
    ESP_LOGI(kTag, "no NVS config; using two default ROMs");
    return ESP_OK;
  }
  if (err != ESP_OK) return err;
  static char json[kMaxConfigJson + 1];
  size_t size = sizeof(json);
  err = nvs_get_str(handle, "config", json, &size);
  nvs_close(handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    *config = ow::defaultConfig();
    ESP_LOGI(kTag, "no config key; using defaults");
    return ESP_OK;
  }
  if (err != ESP_OK) return err;
  char message[128];
  if (!configParse(json, config, message, sizeof(message))) {
    ESP_LOGE(kTag, "invalid NVS configuration: %s", message);
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

esp_err_t configSave(const ow::Config& config) {
  char* json = configJson(config);
  if (!json) return ESP_ERR_NO_MEM;
  nvs_handle_t handle;
  esp_err_t err = nvs_open("ow_emu", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, "config", json);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  cJSON_free(json);
  return err;
}
