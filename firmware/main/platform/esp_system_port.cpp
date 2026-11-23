// SPDX-License-Identifier: MIT
#include "platform/esp_system_port.h"

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "hw/battery.h"
#include "platform/wifi_link.h"
#include "sdkconfig.h"

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external pin";
    case ESP_RST_SW:
      return "software restart";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep sleep wake";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    default:
      return "other";
  }
}

void EspSystemPort::init(WifiLink* wifi) {
  wifi_ = wifi;
  const esp_reset_reason_t rr = esp_reset_reason();
  reset_ = resetName(rr);
  const uint32_t causes = esp_sleep_get_wakeup_causes();
  if (rr != ESP_RST_DEEPSLEEP) {
    wake_ = reset_;
  } else if (causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) {
    wake_ = "timer";
  } else if (causes & (1u << ESP_SLEEP_WAKEUP_EXT1)) {
    wake_ = "service button";
    wokeByButton = true;
  } else if (causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
    wake_ = "gpio";
  } else {
    wake_ = "deep sleep (other)";
  }
  coldBoot = rr != ESP_RST_DEEPSLEEP;
  // An image that failed its first start (rollback) must not be installed
  // again by the remote update.
  const esp_partition_t* invalid = esp_ota_get_last_invalid_partition();
  esp_app_desc_t desc;
  if (invalid != nullptr &&
      esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
    snprintf(rejected_, sizeof(rejected_), "%s", desc.version);
  }
}

int EspSystemPort::batteryMv() {
  // Read once per wake: the 1 MOhm divider needs its capacitor charged.
  if (!batteryRead_) {
    batteryMv_ = batteryReadMv();
    batteryRead_ = true;
  }
  return batteryMv_;
}

static const char* otaStateName(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:
      return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "pending_verify";
    case ESP_OTA_IMG_VALID:
      return "valid";
    case ESP_OTA_IMG_INVALID:
      return "invalid";
    case ESP_OTA_IMG_ABORTED:
      return "aborted";
    default:
      return "undefined";
  }
}

void EspSystemPort::writePlatformJson(gm::JsonWriter& w, bool detailed) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  if (run != nullptr) {
    esp_ota_get_state_partition(run, &st);
  }
  w.key("esp").beginObject();
  w.kvb("service_mode", serviceMode.load())
      .kv("ap_ssid", apSsid)
      .kv("ap_ip", "192.168.4.1")
      .kv("ap_clients",
          static_cast<int64_t>(wifi_ != nullptr ? wifi_->apClients() : 0))
      .kvu("service_idle_s", serviceIdleS.load())
      .kvu("service_timeout_s", CONFIG_GM_SERVICE_TIMEOUT_S)
      .kv("running_partition", run != nullptr ? run->label : "?")
      .kv("next_partition", next != nullptr ? next->label : "?")
      .kvu("slot_size", next != nullptr ? next->size : 0)
      .kv("app_state", otaStateName(st))
      .kvu("free_heap", esp_get_free_heap_size())
      .kv("onewire_gpio", static_cast<int64_t>(CONFIG_GM_ONEWIRE_GPIO))
      .kv("service_wake_gpio",
          static_cast<int64_t>(CONFIG_GM_SERVICE_WAKE_GPIO))
      .kv("rejected_firmware", rejected_);
  if (detailed) {
    w.kvu("min_free_heap", esp_get_minimum_free_heap_size());
    w.kv("idf", esp_get_idf_version());
  }
  w.endObject();
}

bool EspSystemPort::platformAction(const char* action, const gm::Form& args,
                                   char* msg, size_t cap) {
  if (std::strcmp(action, "exit_service") == 0) {
    if (!serviceMode) {
      snprintf(msg, cap, "not in service mode");
      return false;
    }
    exitService = true;
    snprintf(msg, cap,
             "leaving service mode; normal measurement cycle resumes");
    return true;
  }
  if (std::strcmp(action, "enter_service") == 0) {
    long minutes = 0;
    if (args.getInt("minutes", 1, 60, &minutes)) {
      serviceMinutes = static_cast<uint32_t>(minutes);
    }
    enterService = true;
    snprintf(msg, cap, "entering service mode (SoftAP %s, http://192.168.4.1/)",
             apSsid);
    return true;
  }
  if (std::strcmp(action, "sim") == 0) {
    snprintf(msg, cap, "simulation is only available in the PC build");
    return false;
  }
  snprintf(msg, cap, "unknown action");
  return false;
}
