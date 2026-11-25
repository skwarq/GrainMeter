// SPDX-License-Identifier: MIT
#pragma once

#include "app/platform.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Wi-Fi driver owner: station uplink for reporting (gm::Network) and the
// SoftAP used in service mode. All waits are bounded.
class WifiLink : public gm::Network {
 public:
  esp_err_t init();  // netif, event loop, driver (no radio yet)

  bool connect(const gm::Config& cfg, uint32_t timeoutMs, char* err,
               size_t errCap) override;
  void disconnect() override;
  bool syncTime(uint32_t timeoutMs) override;
  int rssi() override;
  const char* stateText() override;
  const char* ipText() override;
  int scan(gm::WifiNetwork* out, int max) override;

  // Service mode on the bench: the station link stays up for the LAN portal,
  // so disconnect() after an upload keeps it. false drops it again.
  void setKeepStation(bool keep);
  bool connected() const { return connected_; }
  // "station": {ip, channel, bssid, rssi} for /api/diag.
  void writeJson(gm::JsonWriter& w);

  // Service mode access point (open when password is empty, else WPA2-PSK),
  // fixed IP 192.168.4.1, DHCP option 114 = portal URL.
  esp_err_t startAp(const char* ssid, const char* password, uint8_t channel);
  void stopAp();
  int apClients();
  bool apRunning() const { return apRunning_; }

 private:
  static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
  esp_err_t applyMode();
  void dropStation();

  esp_netif_t* sta_ = nullptr;
  esp_netif_t* ap_ = nullptr;
  EventGroupHandle_t events_ = nullptr;
  bool started_ = false;
  bool staWanted_ = false;
  bool apRunning_ = false;
  bool scanning_ = false;
  bool keepSta_ = false;
  char staSsid_[33] = "";  // network of the current station link
  char ip_[16] = "";
  volatile bool connected_ = false;
  volatile int lastReason_ = 0;
};
