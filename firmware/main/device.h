// SPDX-License-Identifier: MIT
#pragma once

#include "app/grainmeter.h"
#include "app/portal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hw/onewire_gpio.h"
#include "platform/esp_clock.h"
#include "platform/esp_ota_target.h"
#include "platform/esp_system_port.h"
#include "platform/history_partition.h"
#include "platform/https_client.h"
#include "platform/nvs_store.h"
#include "platform/wifi_link.h"

// Everything the firmware owns, created once in app_main(). Shared logic is
// not thread-safe: the main loop, HTTP handlers and the USB console take
// `lock` around every call into app/portal.
struct Device {
  NvsStore store;
  EspClock clock;
  OneWireGpio bus{static_cast<gpio_num_t>(CONFIG_GM_ONEWIRE_GPIO)};
  WifiLink wifi;
  HttpsClient http;
  EspOtaTarget ota;
  EspSystemPort sys;
  HistoryPartition history;
  gm::GrainMeter* app = nullptr;
  gm::Portal* portal = nullptr;
  SemaphoreHandle_t lock = nullptr;
};

Device& device();

class DeviceLock {
 public:
  DeviceLock() { xSemaphoreTake(device().lock, portMAX_DELAY); }
  ~DeviceLock() { xSemaphoreGive(device().lock); }
  DeviceLock(const DeviceLock&) = delete;
  DeviceLock& operator=(const DeviceLock&) = delete;
};
