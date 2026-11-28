// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "capture.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "services.h"

extern "C" void app_main() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW("sniffer", "reinitializing incompatible/full NVS");
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  ESP_ERROR_CHECK(ow::captureStart());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t* ap = esp_netif_create_default_wifi_ap();
  if (!ap) {
    ESP_LOGE("sniffer", "cannot allocate AP interface");
    abort();
  }
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
  wifi_config_t wifi{};
  std::snprintf(reinterpret_cast<char*>(wifi.ap.ssid), sizeof(wifi.ap.ssid),
                "OW-Sniffer-%02X%02X", mac[4], mac[5]);
  wifi.ap.ssid_len = std::strlen(reinterpret_cast<char*>(wifi.ap.ssid));
  const size_t pass_length = std::strlen(CONFIG_OW_AP_PASSWORD);
  if (pass_length && (pass_length < 8 || pass_length > 63)) {
    ESP_LOGE("sniffer", "AP password must be empty or 8..63 bytes");
    abort();
  }
  std::memcpy(wifi.ap.password, CONFIG_OW_AP_PASSWORD, pass_length);
  wifi.ap.channel = 1;
  wifi.ap.max_connection = 4;
  wifi.ap.authmode = pass_length ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_netif_ip_info_t ip;
  ESP_ERROR_CHECK(esp_netif_get_ip_info(ap, &ip));
  ESP_ERROR_CHECK(ow::dnsStart(ip.ip.addr));
  ESP_ERROR_CHECK(ow::webStart());
  if (xTaskCreate(ow::activityTask, "activity", 2048, nullptr, 1, nullptr) !=
      pdPASS) {
    ESP_LOGE("sniffer", "cannot allocate LED task");
    abort();
  }
  ESP_LOGI("sniffer", "AP %s, http://192.168.4.1/",
           reinterpret_cast<char*>(wifi.ap.ssid));
}
