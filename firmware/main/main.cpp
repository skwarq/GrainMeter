// SPDX-License-Identifier: MIT
// GrainMeter firmware for DFRobot FireBeetle 2 ESP32-C6 (DFR1075).
//
// Normal cycle (battery): wake -> measure (every 15 min) -> upload when due
// (4 samples, hourly) -> deep sleep. Samples wait in RTC memory between
// wakes. Service mode (BOOT pressed within the window after reset, first
// boot without configuration, or `service` on the USB console): WPA2 SoftAP
// "GrainMeter-XXXX", captive DNS, portal at http://192.168.4.1/, OTA upload.
#include <cstdio>
#include <cstring>

#include "device.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw/board_io.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "service/dns_redirect.h"
#include "service/usb_console.h"
#include "service/web_portal.h"
#include "util/log.h"

static const char* kTag = "main";

// Survives deep sleep (RTC memory), not power loss: validated by CRC.
RTC_NOINIT_ATTR static gm::RetainedState s_retained;

static Device s_device;
Device& device() { return s_device; }

static void logSink(gm::LogLevel level, const char* tag, const char* msg) {
  switch (level) {
    case gm::LogLevel::Error:
      ESP_LOGE(tag, "%s", msg);
      break;
    case gm::LogLevel::Warn:
      ESP_LOGW(tag, "%s", msg);
      break;
    case gm::LogLevel::Info:
      ESP_LOGI(tag, "%s", msg);
      break;
    default:
      ESP_LOGD(tag, "%s", msg);
      break;
  }
}

static esp_err_t initNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kTag, "NVS layout changed, erasing");
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  return err;
}

// After an OTA update the new image runs in PENDING_VERIFY state. Confirm it
// only if the basic local components work. A missing probe or no Internet
// is NOT a reason to roll back (those are field conditions, not firmware
// faults); a crash or reset before this point makes the bootloader return
// to the previous image.
static void confirmImage(bool nvsOk, bool busOk, bool wifiOk) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running == nullptr ||
      esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }
  if (nvsOk && busOk && wifiOk) {
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(kTag, "new firmware on %s confirmed (self-check passed)",
             running->label);
  } else {
    ESP_LOGE(kTag, "self-check failed (nvs=%d gpio=%d wifi=%d): rolling back",
             nvsOk, busOk, wifiOk);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

[[noreturn]] static void deepSleep(uint32_t seconds) {
  ledSet(false);
  ESP_LOGI(kTag, "deep sleep for %lu s", static_cast<unsigned long>(seconds));
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
#if CONFIG_GM_SERVICE_WAKE_GPIO >= 0
  // Service button wakes the chip without a reset: RTC samples survive.
  const gpio_num_t button =
      static_cast<gpio_num_t>(CONFIG_GM_SERVICE_WAKE_GPIO);
  rtc_gpio_pullup_en(button);
  rtc_gpio_pulldown_dis(button);
  esp_sleep_enable_ext1_wakeup_io(1ULL << CONFIG_GM_SERVICE_WAKE_GPIO,
                                  ESP_EXT1_WAKEUP_ANY_LOW);
#endif
  esp_deep_sleep_start();
}

[[noreturn]] static void restartNow() {
  vTaskDelay(pdMS_TO_TICKS(800));  // let the HTTP reply leave
  esp_restart();
}

static void ensureServicePassword() {
  DeviceLock lock;
  if (device().app->config().servicePassword[0] != '\0' ||
      CONFIG_GM_DEFAULT_SERVICE_PASSWORD[0] == '\0') {
    return;
  }
  char body[96];
  snprintf(body, sizeof(body), "svc_pass=%s",
           CONFIG_GM_DEFAULT_SERVICE_PASSWORD);
  char err[96];
  if (!device().app->applyConfig(gm::Form(body, std::strlen(body)), err,
                                 sizeof(err))) {
    ESP_LOGW(kTag, "default service password rejected: %s", err);
  }
}

static void runServiceMode() {
  Device& d = device();
  d.sys.serviceMode = true;
  d.sys.exitService = false;
  d.sys.enterService = false;
  char pass[65];
  {
    DeviceLock lock;
    snprintf(pass, sizeof(pass), "%s", d.app->config().servicePassword);
  }
  if (pass[0] == '\0') {
    ESP_LOGW(kTag,
             "service password not set: SoftAP disabled. Use the USB console: "
             "set svc_pass <8+ chars>");
  } else if (d.wifi.startAp(d.sys.apSsid, pass, CONFIG_GM_AP_CHANNEL) ==
             ESP_OK) {
    const uint8_t ipBytes[4] = {192, 168, 4, 1};
    uint32_t ip;
    std::memcpy(&ip, ipBytes, 4);
    dnsRedirectStart(ip);
    webPortalStart();
    ESP_LOGI(kTag, "service mode: SSID %s, http://192.168.4.1/", d.sys.apSsid);
  } else {
    ESP_LOGE(kTag, "SoftAP start failed");
  }
  std::memset(pass, 0, sizeof(pass));
  ledBlink(1000);

  // A remotely requested window stays open at least that long, even before
  // the technician connects; afterwards the normal idle timeout applies.
  const int64_t minOpenUs =
      static_cast<int64_t>(d.sys.serviceMinutes.exchange(0)) * 60 * 1000000;
  const int64_t startUs = esp_timer_get_time();
  int64_t noClientS = 0;
  while (!d.sys.exitService) {
    {
      DeviceLock lock;
      d.app->runDue(false);  // keep measuring; radio stays on the AP
    }
    if (d.sys.restart) {
      restartNow();
    }
    bool otaBusy;
    {
      DeviceLock lock;
      otaBusy = d.app->ota().busy();
    }
    const int clients = d.wifi.apClients();
    noClientS = clients > 0 ? 0 : noClientS + 1;
    const uint32_t httpIdle = webPortalIdleSeconds();
    const uint32_t usbIdle =
        usbConnected() ? usbConsoleIdleSeconds() : UINT32_MAX;
    const uint32_t idle = static_cast<uint32_t>(noClientS) < httpIdle
                              ? static_cast<uint32_t>(noClientS)
                              : httpIdle;
    d.sys.serviceIdleS = idle;
    // Never leave (and never sleep) while an update is being written.
    const bool windowOver = esp_timer_get_time() - startUs >= minOpenUs;
    if (!otaBusy && windowOver && idle >= CONFIG_GM_SERVICE_TIMEOUT_S &&
        usbIdle >= CONFIG_GM_SERVICE_TIMEOUT_S) {
      ESP_LOGI(kTag, "service mode idle for %lu s",
               static_cast<unsigned long>(idle));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  webPortalStop();
  dnsRedirectStop();
  d.wifi.stopAp();
  ledBlink(0);
  d.sys.serviceMode = false;
  d.sys.exitService = false;
  ESP_LOGI(kTag, "service mode finished, back to the measurement cycle");
}

extern "C" void app_main(void) {
  gm::setLogSink(logSink);
  Device& d = device();
  d.lock = xSemaphoreCreateMutex();
  boardIoInit();

  const bool nvsOk = initNvs() == ESP_OK && d.store.open() == ESP_OK;
  const bool busOk = d.bus.init() == ESP_OK;
  const bool wifiOk = d.wifi.init() == ESP_OK;
  d.sys.init(&d.wifi);
  confirmImage(nvsOk, busOk, wifiOk);
  if (busOk && !d.bus.lineIdleHigh()) {
    ESP_LOGW(kTag,
             "1-Wire DATA (GPIO%d) is low while idle: missing pull-up or short",
             CONFIG_GM_ONEWIRE_GPIO);
  }

  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(d.sys.apSsid, sizeof(d.sys.apSsid), "GrainMeter-%02X%02X", mac[4],
           mac[5]);

  gm::Ports ports;
  ports.bus = &d.bus;
  ports.clock = &d.clock;
  ports.store = &d.store;
  ports.net = &d.wifi;
  ports.http = &d.http;
  ports.sys = &d.sys;
  ports.ota = &d.ota;
  gm::ConfigLimits limits;
  limits.minMeasureS = 60;
  limits.allowInsecureUrl = false;  // ThingSpeak only over verified HTTPS
  d.app = new gm::GrainMeter(ports, &s_retained, limits,
                             "https://api.thingspeak.com");
  d.portal = new gm::Portal(*d.app);
  {
    DeviceLock lock;
    d.app->begin();
  }
  ensureServicePassword();
  usbConsoleStart();
  ESP_LOGI(kTag, "wake: %s, reset: %s", d.sys.wakeReason(),
           d.sys.resetReason());

  bool service = d.sys.wokeByButton;
  if (service) {
    ESP_LOGI(kTag, "woken by the service button");
  }
  if (d.sys.coldBoot) {
    service = waitForServiceButton(CONFIG_GM_SERVICE_BUTTON_WINDOW_MS);
    DeviceLock lock;
    if (d.app->config().wifiSsid[0] == '\0') {
      ESP_LOGI(kTag, "not configured yet: starting service mode");
      service = true;
    }
  }

  const int64_t bootUs = esp_timer_get_time();
  while (true) {
    if (service || d.sys.enterService || serviceWakeButtonPressed()) {
      runServiceMode();
      service = false;
    }
    uint32_t sleepS;
    {
      DeviceLock lock;
      sleepS = d.app->runDue(true);
    }
    if (d.sys.restart) {
      restartNow();
    }
    // Stay awake while somebody uses the USB console (first 10 s after a
    // cold boot, then 60 s after each command); otherwise sleep at once.
    const bool usbActive =
        usbConnected() &&
        (usbConsoleIdleSeconds() < 60 ||
         (d.sys.coldBoot && esp_timer_get_time() - bootUs < 10000000));
    if (usbActive || d.sys.enterService) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    deepSleep(sleepS);
  }
}
