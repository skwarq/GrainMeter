// SPDX-License-Identifier: MIT
#include "platform/wifi_link.h"

#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "platform/power_stats.h"
#include "service/session_hold.h"

static const char* kTag = "wifi";
static const EventBits_t kGotIp = BIT0;
static const EventBits_t kFailed = BIT1;

esp_err_t WifiLink::init() {
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK) return err;
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  sta_ = esp_netif_create_default_wifi_sta();
  ap_ = esp_netif_create_default_wifi_ap();  // default IP 192.168.4.1/24 + DHCP
                                             // server
  events_ = xEventGroupCreate();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK) return err;
  esp_wifi_set_storage(
      WIFI_STORAGE_RAM);  // credentials live in our NVS namespace
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiLink::onEvent,
                             this);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiLink::onEvent,
                             this);
  return ESP_OK;
}

void WifiLink::onEvent(void* arg, esp_event_base_t base, int32_t id,
                       void* data) {
  WifiLink* self = static_cast<WifiLink*>(arg);
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t* d =
        static_cast<wifi_event_sta_disconnected_t*>(data);
    self->connected_ = false;
    self->lastReason_ = d->reason;
    xEventGroupSetBits(self->events_, kFailed);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    self->connected_ = true;
    xEventGroupSetBits(self->events_, kGotIp);
  }
}

esp_err_t WifiLink::applyMode() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  const bool sta = staWanted_ || scanning_;
  if (sta && apRunning_) {
    mode = WIFI_MODE_APSTA;
  } else if (sta) {
    mode = WIFI_MODE_STA;
  } else if (apRunning_) {
    mode = WIFI_MODE_AP;
  }
  if (mode == WIFI_MODE_NULL) {
    if (started_) {
      esp_wifi_stop();
      started_ = false;
    }
    return ESP_OK;
  }
  esp_err_t err = esp_wifi_set_mode(mode);
  if (err == ESP_OK && !started_) {
    err = esp_wifi_start();
    started_ = err == ESP_OK;
  }
  return err;
}

// Last successful access point, kept in RTC memory across deep sleep:
// connecting with a known channel + BSSID skips the full channel scan.
struct ApCache {
  uint32_t magic;
  char ssid[33];
  uint8_t bssid[6];
  uint8_t channel;
};
RTC_DATA_ATTR static ApCache s_apCache;
static const uint32_t kApCacheMagic = 0x41504331;  // "APC1"

static esp_err_t applyStaticIp(esp_netif_t* netif, const gm::Config& cfg) {
  if (!cfg.staticIpEnabled()) {
    esp_netif_dhcpc_start(netif);  // "already started" is fine
    return ESP_OK;
  }
  uint8_t ip[4], gw[4], mask[4];
  if (!gm::parseIpv4(cfg.staticIp, ip) || !gm::parseIpv4(cfg.gateway, gw) ||
      !gm::parseIpv4(cfg.netmask, mask)) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_netif_dhcpc_stop(netif);
  esp_netif_ip_info_t info = {};
  info.ip.addr = ESP_IP4TOADDR(ip[0], ip[1], ip[2], ip[3]);
  info.gw.addr = ESP_IP4TOADDR(gw[0], gw[1], gw[2], gw[3]);
  info.netmask.addr = ESP_IP4TOADDR(mask[0], mask[1], mask[2], mask[3]);
  esp_err_t err = esp_netif_set_ip_info(netif, &info);
  uint8_t dns[4];
  const char* dnsText = cfg.dns[0] != '\0' ? cfg.dns : cfg.gateway;
  if (err == ESP_OK && gm::parseIpv4(dnsText, dns)) {
    esp_netif_dns_info_t d = {};
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = ESP_IP4TOADDR(dns[0], dns[1], dns[2], dns[3]);
    err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &d);
  }
  return err;
}

bool WifiLink::connect(const gm::Config& cfg, uint32_t timeoutMs, char* err,
                       size_t errCap) {
  if (cfg.wifiSsid[0] == '\0') {
    snprintf(err, errCap, "Wi-Fi SSID not configured");
    return false;
  }
  if (connected_ && std::strcmp(staSsid_, cfg.wifiSsid) == 0) {
    return true;  // link kept for the LAN portal
  }
  if (connected_) {
    // Kept link to another network (portal join test): leave it first.
    xEventGroupClearBits(events_, kFailed);
    esp_wifi_disconnect();
    xEventGroupWaitBits(events_, kFailed, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    connected_ = false;
  }
  if (applyStaticIp(sta_, cfg) != ESP_OK) {
    snprintf(err, errCap, "invalid static IP configuration");
    return false;
  }
  powerStatsWifiStart();
  staWanted_ = true;
  if (applyMode() != ESP_OK) {
    snprintf(err, errCap, "Wi-Fi start failed");
    return false;
  }
  bool useCache = s_apCache.magic == kApCacheMagic &&
                  std::strcmp(s_apCache.ssid, cfg.wifiSsid) == 0 &&
                  s_apCache.channel >= 1 && s_apCache.channel <= 13;

  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
  lastReason_ = 0;
  while (xTaskGetTickCount() < deadline) {
    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), cfg.wifiSsid,
                 sizeof(wc.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(wc.sta.password), cfg.wifiPass,
                 sizeof(wc.sta.password));
    wc.sta.threshold.authmode =
        cfg.wifiPass[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    if (useCache) {
      wc.sta.channel = s_apCache.channel;
      wc.sta.bssid_set = true;
      std::memcpy(wc.sta.bssid, s_apCache.bssid, sizeof(wc.sta.bssid));
    }
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    xEventGroupClearBits(events_, kGotIp | kFailed);
    esp_wifi_connect();
    const TickType_t left = deadline - xTaskGetTickCount();
    const EventBits_t bits =
        xEventGroupWaitBits(events_, kGotIp | kFailed, pdTRUE, pdFALSE, left);
    if (bits & kGotIp) {
      powerStatsWifiConnected();
      wifi_ap_record_t ap = {};
      if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_apCache.magic = kApCacheMagic;
        std::snprintf(s_apCache.ssid, sizeof(s_apCache.ssid), "%s",
                      cfg.wifiSsid);
        std::memcpy(s_apCache.bssid, ap.bssid, sizeof(s_apCache.bssid));
        s_apCache.channel = ap.primary;
      }
      std::snprintf(staSsid_, sizeof(staSsid_), "%s", cfg.wifiSsid);
      esp_netif_ip_info_t info = {};
      ip_[0] = '\0';
      if (esp_netif_get_ip_info(sta_, &info) == ESP_OK) {
        std::snprintf(ip_, sizeof(ip_), IPSTR, IP2STR(&info.ip));
      }
      ESP_LOGI(kTag, "connected to '%s' (ch %d%s%s), RSSI %d dBm, IP %s",
               cfg.wifiSsid, ap.primary, useCache ? ", cached AP" : "",
               cfg.staticIpEnabled() ? ", static IP" : "", rssi(), ip_);
      return true;
    }
    if (useCache) {
      // AP moved to another channel or was replaced: forget it, scan.
      ESP_LOGI(kTag, "cached AP not reachable, full scan");
      s_apCache.magic = 0;
      useCache = false;
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(500));  // retry until the deadline
  }
  switch (lastReason_) {
    case WIFI_REASON_NO_AP_FOUND:
      snprintf(err, errCap, "network '%.32s' not found (2.4 GHz only)",
               cfg.wifiSsid);
      break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
      snprintf(err, errCap, "wrong Wi-Fi password for '%.32s' (reason %d)",
               cfg.wifiSsid, lastReason_);
      break;
    default:
      snprintf(err, errCap, "no connection to '%.32s' within %lu s (reason %d)",
               cfg.wifiSsid, static_cast<unsigned long>(timeoutMs / 1000u),
               lastReason_);
      break;
  }
  dropStation();
  return false;
}

void WifiLink::dropStation() {
  if (staWanted_) {
    esp_wifi_disconnect();
  }
  staWanted_ = false;
  connected_ = false;
  staSsid_[0] = '\0';
  ip_[0] = '\0';
  applyMode();
  powerStatsWifiEnd();
}

void WifiLink::disconnect() {
  if (connected_ && dropHook_ != nullptr) {
    dropHook_();
  }
  if ((keepSta_ || holdActive()) && connected_) {
    return;  // LAN portal keeps using the link
  }
  dropStation();
}

void WifiLink::setKeepStation(bool keep) {
  keepSta_ = keep;
  if (!keep && !holdActive()) {
    dropStation();
  }
}

const char* WifiLink::ipText() { return connected_ ? ip_ : ""; }

int WifiLink::scan(gm::WifiNetwork* out, int max) {
  scanning_ = true;
  if (applyMode() != ESP_OK) {
    scanning_ = false;
    applyMode();
    return 0;
  }
  wifi_scan_config_t sc = {};
  sc.show_hidden = false;
  esp_err_t err = esp_wifi_scan_start(&sc, true);  // blocks ~2-3 s
  int n = 0;
  if (err == ESP_OK) {
    static wifi_ap_record_t recs[40];  // static: HTTP task stack is small
    uint16_t count = static_cast<uint16_t>(max < 40 ? max : 40);
    if (esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
      for (uint16_t i = 0; i < count; ++i) {
        std::snprintf(out[n].ssid, sizeof(out[n].ssid), "%s",
                      reinterpret_cast<const char*>(recs[i].ssid));
        out[n].rssi = recs[i].rssi;
        out[n].channel = recs[i].primary;
        out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        ++n;
      }
    }
  } else {
    ESP_LOGW(kTag, "scan failed: %s", esp_err_to_name(err));
    esp_wifi_clear_ap_list();
  }
  scanning_ = false;
  applyMode();
  ESP_LOGI(kTag, "scan: %d network(s)", n);
  return n;
}

bool WifiLink::syncTime(uint32_t timeoutMs) {
  {
    esp_netif_dns_info_t dns = {};
    esp_netif_get_dns_info(sta_, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_t* def = esp_netif_get_default_netif();
    ESP_LOGI(kTag, "sntp: default netif %s, sta dns " IPSTR,
             def != nullptr ? esp_netif_get_desc(def) : "none",
             IP2STR(&dns.ip.u_addr.ip4));
  }
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  if (esp_netif_sntp_init(&cfg) != ESP_OK) {
    return false;
  }
  const int64_t t0 = esp_timer_get_time();
  const bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeoutMs)) == ESP_OK;
  powerStatsSntp(static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000));
  esp_netif_sntp_deinit();
  ESP_LOGI(kTag, "SNTP %s", ok ? "synchronised" : "timeout");
  return ok;
}

int WifiLink::rssi() {
  int value = 0;
  if (!connected_ || esp_wifi_sta_get_rssi(&value) != ESP_OK) {
    return 0;
  }
  return value;
}

const char* WifiLink::stateText() {
  if (connected_) return apRunning_ ? "connected (+service AP)" : "connected";
  if (staWanted_) return "connecting";
  return apRunning_ ? "service AP" : "off";
}

esp_err_t WifiLink::startAp(const char* ssid, const char* password,
                            uint8_t channel) {
  wifi_config_t wc = {};
  std::strncpy(reinterpret_cast<char*>(wc.ap.ssid), ssid, sizeof(wc.ap.ssid));
  wc.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
  std::strncpy(reinterpret_cast<char*>(wc.ap.password), password,
               sizeof(wc.ap.password));
  wc.ap.channel = channel;
  wc.ap.max_connection = 4;
  const bool open = password == nullptr || password[0] == '\0';
  wc.ap.authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  wc.ap.pmf_cfg.capable = !open;
  // DHCP option 114 (RFC 8910): Android 11+ / iOS 14+ learn the portal
  // address from the lease and open it without probing.
  static char portalUri[] = "http://192.168.4.1/";
  esp_netif_dhcps_stop(ap_);
  esp_netif_dhcps_option(ap_, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                         portalUri, std::strlen(portalUri));
  esp_netif_dhcps_start(ap_);
  apRunning_ = true;
  esp_err_t err = applyMode();
  if (err == ESP_OK) {
    err = esp_wifi_set_config(WIFI_IF_AP, &wc);
  }
  if (err != ESP_OK) {
    apRunning_ = false;
    applyMode();
  }
  return err;
}

void WifiLink::stopAp() {
  apRunning_ = false;
  applyMode();
}

int WifiLink::apClients() {
  wifi_sta_list_t list = {};
  if (!apRunning_ || esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
    return 0;
  }
  return list.num;
}

void WifiLink::writeJson(gm::JsonWriter& w) {
  w.key("station").beginObject().kvb("connected", connected_);
  wifi_ap_record_t ap = {};
  if (connected_ && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    char bssid[18];
    std::snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                  ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3],
                  ap.bssid[4], ap.bssid[5]);
    w.kv("ssid", staSsid_)
        .kv("ip", ip_)
        .kvu("channel", ap.primary)
        .kv("bssid", bssid)
        .kv("rssi", static_cast<int64_t>(ap.rssi))
        .kvb("kept_for_lan", keepSta_);
  }
  w.endObject();
}
