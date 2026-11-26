// SPDX-License-Identifier: MIT
#include "platform/esp_system_port.h"

#include <cstdio>
#include <cstring>

#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw/battery.h"
#include "nvs.h"
#include "platform/power_stats.h"
#include "platform/wifi_link.h"
#include "sdkconfig.h"
#include "service/remote_session.h"
#include "service/session_hold.h"
#include "service/udp_log.h"

static portMUX_TYPE s_nameMux = portMUX_INITIALIZER_UNLOCKED;

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
  // Boot counter (cold starts only, not deep sleep wakes), like WIFI_RS.
  nvs_handle_t h;
  if (nvs_open("gm_diag", NVS_READWRITE, &h) == ESP_OK) {
    nvs_get_u32(h, "boots", &bootCount_);
    if (coldBoot) {
      ++bootCount_;
      nvs_set_u32(h, "boots", bootCount_);
      nvs_commit(h);
    }
    nvs_close(h);
  }
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

int32_t EspSystemPort::batteryMahPerDayCenti() {
  return powerStatsMahPerDayCenti();
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
  remoteWriteJson(w);
  if (detailed) {
    w.kvu("min_free_heap", esp_get_minimum_free_heap_size());
    w.kv("idf", esp_get_idf_version());
    writeDetails(w);
  }
  w.endObject();
}

// Chip temperature (die, not ambient: Wi-Fi and CPU warm it up).
static bool chipTempC(float* out) {
  static temperature_sensor_handle_t s_ts = nullptr;
  if (s_ts == nullptr) {
    temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&c, &s_ts) != ESP_OK ||
        temperature_sensor_enable(s_ts) != ESP_OK) {
      s_ts = nullptr;
      return false;
    }
  }
  return temperature_sensor_get_celsius(s_ts, out) == ESP_OK;
}

// WIFI_RS style diagnostics: uptime, boots, build, chip temperature, heap,
// FreeRTOS tasks, station link, UDP log counters, last crash.
void EspSystemPort::writeDetails(gm::JsonWriter& w) {
  const esp_app_desc_t* app = esp_app_get_description();
  char build[40];
  snprintf(build, sizeof(build), "%s %s", app->date, app->time);
  w.kvu("uptime_s", static_cast<uint32_t>(esp_timer_get_time() / 1000000))
      .kvu("boot_count", bootCount_)
      .kv("build", build);
  float t = 0;
  if (chipTempC(&t)) {
    w.key("chip_temp_c").centi(static_cast<int32_t>(t * 100.0f));
  } else {
    w.key("chip_temp_c").null();
  }
  multi_heap_info_t hi = {};
  heap_caps_get_info(&hi, MALLOC_CAP_DEFAULT);
  w.key("heap")
      .beginObject()
      .kvu("free", hi.total_free_bytes)
      .kvu("allocated", hi.total_allocated_bytes)
      .kvu("min_free", hi.minimum_free_bytes)
      .kvu("largest_block", hi.largest_free_block)
      .endObject();
  if (wifi_ != nullptr) {
    wifi_->writeJson(w);
  }
  w.key("udp_log")
      .beginObject()
      .kvu("sent", udpLogSent())
      .kvu("dropped", udpLogDropped())
      .endObject();
  w.key("logs")
      .beginObject()
      .kv("log_dest", logDestinationMqtt() ? "mqtt" : "udp")
      .kvu("mqtt_sent", mqttLogSent())
      .kvu("mqtt_dropped", mqttLogDropped())
      .endObject();
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
  static TaskStatus_t tasks[32];
  const UBaseType_t n = uxTaskGetSystemState(tasks, 32, nullptr);
  w.key("tasks").beginArray();
  for (UBaseType_t i = 0; i < n; ++i) {
    w.beginObject()
        .kv("name", tasks[i].pcTaskName)
        .kvu("stack_free", static_cast<uint32_t>(tasks[i].usStackHighWaterMark *
                                                 sizeof(StackType_t)))
        .kvu("prio", static_cast<uint32_t>(tasks[i].uxCurrentPriority))
        .endObject();
  }
  w.endArray();
#endif
  writeCrash(w);
  powerStatsWrite(w);
}

// Last crash from the core dump partition (like the WIFI_RS crash log):
// read once per wake, kept until cleared from the portal.
void EspSystemPort::writeCrash(gm::JsonWriter& w) {
  static bool s_read = false;
  static bool s_present = false;
  static esp_core_dump_summary_t s_sum;
  static char s_reason[96] = "";
  if (!s_read) {
    s_read = true;
    s_present = esp_core_dump_image_check() == ESP_OK &&
                esp_core_dump_get_summary(&s_sum) == ESP_OK;
    if (s_present &&
        esp_core_dump_get_panic_reason(s_reason, sizeof(s_reason)) != ESP_OK) {
      s_reason[0] = '\0';
    }
  }
  if (crashCleared_) {
    s_present = false;
  }
  w.key("crash").beginObject().kvb("present", s_present);
  if (s_present) {
    char pc[12];
    char cause[12];
    char elf[17];
    snprintf(pc, sizeof(pc), "0x%08lx",
             static_cast<unsigned long>(s_sum.exc_pc));
    snprintf(cause, sizeof(cause), "%lu",
             static_cast<unsigned long>(s_sum.ex_info.mcause));
    snprintf(elf, sizeof(elf), "%.16s",
             reinterpret_cast<const char*>(s_sum.app_elf_sha256));
    w.kv("task", s_sum.exc_task)
        .kv("pc", pc)
        .kv("mcause", cause)
        .kv("reason", s_reason)
        .kv("elf_sha256", elf);
  }
  w.endObject();
}

bool EspSystemPort::boardTempCenti(int16_t* out) {
  float t = 0;
  if (!chipTempC(&t)) return false;
  *out = static_cast<int16_t>(t * 100.0f);
  return true;
}

gm::LogSource* EspSystemPort::logSource() { return logPreviewSource(); }

void EspSystemPort::configApplied(const gm::Config& cfg) {
  udpLogApply(cfg.logUdp != 0, cfg.logLevel, cfg.logHost,
              static_cast<uint16_t>(cfg.logPort), apSsid, cfg.logDest == 1);
  lanWake = cfg.lanWake != 0;
  portENTER_CRITICAL(&s_nameMux);
  snprintf(deviceName, sizeof(deviceName), "%s", cfg.deviceName);
  portEXIT_CRITICAL(&s_nameMux);
}

void EspSystemPort::copyDeviceName(char* out, size_t cap) {
  portENTER_CRITICAL(&s_nameMux);
  snprintf(out, cap, "%s", deviceName);
  portEXIT_CRITICAL(&s_nameMux);
}

bool EspSystemPort::platformAction(const char* action, const gm::Form& args,
                                   char* msg, size_t cap) {
  if (std::strcmp(action, "exit_service") == 0) {
    if (!serviceMode && !holdActive(HoldOwner::Remote)) {
      snprintf(msg, cap, "not in service mode");
      return false;
    }
    exitService = true;
    remoteStop();
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
  if (std::strcmp(action, "crash_clear") == 0) {
    if (esp_core_dump_image_erase() != ESP_OK) {
      snprintf(msg, cap, "crash record could not be erased");
      return false;
    }
    crashCleared_ = true;
    snprintf(msg, cap, "crash record erased");
    return true;
  }
  if (std::strcmp(action, "sim") == 0) {
    snprintf(msg, cap, "simulation is only available in the PC build");
    return false;
  }
  snprintf(msg, cap, "unknown action");
  return false;
}
