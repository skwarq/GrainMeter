// SPDX-License-Identifier: MIT
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "config_store.h"
#include "dns_redirect.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "real_sensor.h"
#include "slave_bus.h"
#include "web_portal.h"

namespace {
void startAp() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t* ap = esp_netif_create_default_wifi_ap();
  if (!ap) ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
  wifi_config_t config = {};
  std::snprintf(reinterpret_cast<char*>(config.ap.ssid), sizeof(config.ap.ssid),
                "OW-Emu-%02X%02X", mac[4], mac[5]);
  const size_t passwordLength = std::strlen(CONFIG_OW_AP_PASSWORD);
  if (passwordLength != 0 && (passwordLength < 8 || passwordLength > 63))
    ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
  std::memcpy(config.ap.password, CONFIG_OW_AP_PASSWORD, passwordLength);
  config.ap.ssid_len =
      std::strlen(reinterpret_cast<const char*>(config.ap.ssid));
  config.ap.channel = 1;
  config.ap.max_connection = 4;
  config.ap.authmode = passwordLength ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  esp_netif_ip_info_t ip;
  ESP_ERROR_CHECK(esp_netif_get_ip_info(ap, &ip));
  ESP_ERROR_CHECK(dnsRedirectStart(ip.ip.addr));
  ESP_LOGI("ow_emu", "AP %s, http://192.168.4.1/, slave GPIO%d",
           reinterpret_cast<const char*>(config.ap.ssid), CONFIG_OW_SLAVE_GPIO);
}
}  // namespace

extern "C" void app_main() {
  // Do not silently erase a corrupt NVS partition containing diagnostic data.
  ESP_ERROR_CHECK(nvs_flash_init());
  ow::Config config;
  ESP_ERROR_CHECK(configLoad(&config));
  startAp();
  ESP_ERROR_CHECK(slaveBusStart(config));
  ESP_ERROR_CHECK(realSensorStart());
  ESP_ERROR_CHECK(webPortalStart(config));
  uint32_t previousLate = 0;
  uint32_t previousOverruns = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const BusStats stats = slaveBusStats();
    if (stats.lateReactions != previousLate ||
        stats.overruns != previousOverruns) {
      ESP_LOGW("timing",
               "late=%" PRIu32 " max=%.3f us overruns=%" PRIu32
               " first_edges=%" PRIu32,
               stats.lateReactions, stats.maxReactionCycles / 160.0,
               stats.overruns, stats.firstEdges);
      previousLate = stats.lateReactions;
      previousOverruns = stats.overruns;
    }
  }
}
