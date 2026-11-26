// SPDX-License-Identifier: MIT
// GrainMeter firmware for DFRobot FireBeetle 2 ESP32-C6 (DFR1075).
//
// Normal cycle (battery): wake -> measure (every 15 min) -> upload when due
// (4 samples, hourly) -> deep sleep. Samples wait in RTC memory between
// wakes. Service mode (BOOT pressed within the window after reset, first
// boot without configuration, `service` on the USB console or a computer on
// USB): open SoftAP "GrainMeter-XXXX" (WPA2 with GM_SERVICE_AP_OPEN=n),
// captive DNS, portal at http://192.168.4.1/ (and on the local network while
// the station link is up), OTA upload.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "device.h"
#include "driver/rtc_io.h"
#include "esp_app_desc.h"
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
#include "platform/power_stats.h"
#include "sdkconfig.h"
#include "service/dns_redirect.h"
#include "service/lan_console.h"
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
  const uint32_t awakeMs = powerStatsSleep(seconds);
  ESP_LOGI(
      kTag, "deep sleep for %lu s (awake %lu ms, boot %lu ms, Wi-Fi %lu ms)",
      static_cast<unsigned long>(seconds), static_cast<unsigned long>(awakeMs),
      static_cast<unsigned long>(powerStatsLastBootMs()),
      static_cast<unsigned long>(powerStatsLastWifiMs()));
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
#if CONFIG_GM_SERVICE_WAKE_GPIO >= 0
  // Service button wakes the chip without a reset: RTC samples survive.
  const gpio_num_t button =
      static_cast<gpio_num_t>(CONFIG_GM_SERVICE_WAKE_GPIO);
  // The pin must be an LP (RTC) IO for its pull-up to work in deep sleep
  // (ESP-IDF ext_wakeup example). Without rtc_gpio_init() an unconnected
  // GPIO2 floats while asleep and wakes the chip at once as if the button
  // had been pressed (service mode, LED blinking, on battery).
  rtc_gpio_init(button);
  rtc_gpio_set_direction(button, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_dis(button);
  rtc_gpio_pullup_en(button);
  esp_sleep_enable_ext1_wakeup_io(1ULL << CONFIG_GM_SERVICE_WAKE_GPIO,
                                  ESP_EXT1_WAKEUP_ANY_LOW);
#endif
  esp_deep_sleep_start();
}

[[noreturn]] static void restartNow() {
  lanServerClose();  // no new console client in the last moments
  if (lanSessionActive()) {
    // OTA or "restart" from the LAN console: announce again after the boot
    // so the PC script reconnects by itself.
    lanSessionEnd("restart");
    lanMarkResume();
  }
  vTaskDelay(pdMS_TO_TICKS(800));  // let the HTTP reply leave
  esp_restart();
}

static LanAnnounce lanAnnounce() {
  Device& d = device();
  static char name[sizeof(d.sys.deviceName)];
  d.sys.copyDeviceName(name, sizeof(name));  // no torn name in the hello
  LanAnnounce a;
  a.ssid = d.sys.apSsid;
  a.name = name;
  a.ip = d.wifi.ipText();
  a.fw = esp_app_get_description()->version;
  return a;
}

// Radio outside the scheduled windows (after RST, resume) only with enough
// battery: a weak cell must not end in a Wi-Fi brownout reset loop.
static bool batteryAllowsRadio() {
  Device& d = device();
  const int mv = d.sys.batteryMv();
  DeviceLock lock;
  const uint32_t low = d.app->config().battLowMv;
  return mv < 0 || low == 0 || static_cast<uint32_t>(mv) >= low ||
         usbConnected();
}

// A client connected: wait (without the device lock) until it logged in or
// gave up; the session task closes silent clients after 10 s.
static bool lanWaitLogin() {
  while (lanClientConnected() && !lanSessionActive()) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return lanSessionActive();
}

// End of an upload window (Wi-Fi still up): offer the LAN console for a
// moment. Runs inside the upload, i.e. with the device lock held.
static void lanWakeHook() {
  Device& d = device();
  if (!d.sys.lanWake || d.sys.serviceMode) {
    return;
  }
  if (lanOffer(lanAnnounce(), 500)) {
    ESP_LOGI(kTag, "LAN console client: staying awake while it logs in");
    d.wifi.setKeepStation(true);
  }
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

// SoftAP + captive DNS. Returns false while no service password is set
// (checked again every second: `set svc_pass` starts it without a restart).
static bool startServiceAp() {
  Device& d = device();
  char pass[65];
  {
    DeviceLock lock;
    snprintf(pass, sizeof(pass), "%s", d.app->config().servicePassword);
  }
#if CONFIG_GM_SERVICE_AP_OPEN
  pass[0] = '\0';  // open AP like WIFI_RS; portal writes still need it
#else
  if (pass[0] == '\0') {
    return false;
  }
#endif
  const bool ok =
      d.wifi.startAp(d.sys.apSsid, pass, CONFIG_GM_AP_CHANNEL) == ESP_OK;
  std::memset(pass, 0, sizeof(pass));
  if (!ok) {
    ESP_LOGE(kTag, "SoftAP start failed");
    return false;
  }
  const uint8_t ipBytes[4] = {192, 168, 4, 1};
  uint32_t ip;
  std::memcpy(&ip, ipBytes, 4);
  dnsRedirectStart(ip);
  ESP_LOGI(kTag, "service mode: SSID %s, http://192.168.4.1/", d.sys.apSsid);
  return true;
}

// Station link for the portal on the local network (bench with a computer
// on USB, or after a join from the portal). Blocks up to 15 s.
static void connectLan() {
  Device& d = device();
  gm::Config cfg;
  {
    DeviceLock lock;
    cfg = d.app->config();
  }
  if (cfg.wifiSsid[0] != '\0') {
    d.wifi.setKeepStation(true);
    char err[96] = "";
    if (d.wifi.connect(cfg, 15000, err, sizeof(err))) {
      ESP_LOGI(kTag, "portal on the local network: http://%s/",
               d.wifi.ipText());
    } else {
      ESP_LOGW(kTag, "local network: %s", err);
    }
  }
  std::memset(&cfg, 0, sizeof(cfg));
}

// Wi-Fi join requested in the portal: connect outside the device lock (the
// portal keeps polling the result meanwhile).
static void runWifiJoin() {
  Device& d = device();
  gm::Config cand;
  {
    DeviceLock lock;
    if (!d.app->wifiJoinTake(&cand)) {
      return;
    }
  }
  d.wifi.setKeepStation(true);
  char err[96] = "";
  const bool ok =
      d.wifi.connect(cand, gm::kWifiConnectTimeoutMs, err, sizeof(err));
  {
    DeviceLock lock;
    d.app->wifiJoinFinish(cand, ok, err);
  }
  std::memset(&cand, 0, sizeof(cand));
  if (ok) {
    ESP_LOGI(kTag, "portal on the local network: http://%s/", d.wifi.ipText());
  }
}

// withAp=false: LAN console session only (no SoftAP); ends with the session.
static void runServiceMode(bool withAp) {
  Device& d = device();
  d.sys.serviceMode = true;
  d.sys.exitService = false;
  d.sys.enterService = false;
  bool apEnabled;
  bool wifiSet;
  uint32_t apDelayS;
  {
    DeviceLock lock;
    apEnabled = d.app->config().apEnabled != 0;
    apDelayS = d.app->config().apDelayS;
    wifiSet = d.app->config().wifiSsid[0] != '\0';
  }
  // Target Wi-Fi first: the SoftAP (captive portal) is the fallback when no
  // station link comes up within ap_delay_s. ap_enabled=0: never.
  const bool apWanted = withAp && apEnabled;
  const int64_t apAtUs =
      esp_timer_get_time() +
      (wifiSet ? static_cast<int64_t>(apDelayS) * 1000000 : 0);
  bool apUp = false;
  if (withAp && !apEnabled) {
    ESP_LOGW(kTag, "SoftAP disabled (ap_enabled=0): portal only over Wi-Fi");
  } else if (apWanted && wifiSet && apDelayS > 0) {
    ESP_LOGI(kTag, "service mode: Wi-Fi first, SoftAP after %lu s without it",
             static_cast<unsigned long>(apDelayS));
  }
  bool apWarned = false;
  webPortalStart();  // also reachable over the station link
  ledBlink(1000);

  // A remotely requested window stays open at least that long, even before
  // the technician connects; afterwards the normal idle timeout applies.
  const int64_t minOpenUs =
      static_cast<int64_t>(d.sys.serviceMinutes.exchange(0)) * 60 * 1000000;
  const int64_t startUs = esp_timer_get_time();
  int64_t lastLanTryUs = -60 * 1000000LL;
  int64_t noClientS = 0;
  while (true) {
    if (d.sys.exitService) {
      if (withAp) break;
      d.sys.exitService = false;  // LAN-only: the console session ends it
    }
    if (apWanted && !apUp && !d.wifi.connected() &&
        esp_timer_get_time() >= apAtUs) {
      apUp = startServiceAp();
      if (!apUp && !apWarned) {
        apWarned = true;
        ESP_LOGW(kTag,
                 "service password not set: SoftAP disabled. Use the USB "
                 "console: set svc_pass <8+ chars>");
      }
    }
    // Computer on USB (bench) or LAN console: portal also on the local
    // network, no timeout.
    const bool bench = usbConnected();
    const bool lanHold = lanSessionActive();
    if (!withAp && !lanHold) {
      break;  // LAN console closed: the main loop decides what comes next
    }
    // Station retries also while the SoftAP is not up yet; once it serves
    // clients, retries (channel scans) would disturb them, except on the
    // bench.
    if ((bench || !apUp) && !d.wifi.connected() &&
        esp_timer_get_time() - lastLanTryUs >= 60 * 1000000LL) {
      lastLanTryUs = esp_timer_get_time();
      connectLan();
    }
    runWifiJoin();
    if (d.wifi.connected()) {
      lanPoll(lanAnnounce(), 5000);  // script can (re)connect any time
    }
    {
      DeviceLock lock;
      // Uploads only over a kept station link; otherwise the radio stays
      // on the AP.
      d.app->runDue(d.wifi.connected());
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
    const uint32_t idle = static_cast<uint32_t>(noClientS) < httpIdle
                              ? static_cast<uint32_t>(noClientS)
                              : httpIdle;
    d.sys.serviceIdleS = idle;
    // Never leave (and never sleep) while an update is being written.
    const bool windowOver = esp_timer_get_time() - startUs >= minOpenUs;
    if (!otaBusy && !bench && !lanHold && windowOver &&
        idle >= CONFIG_GM_SERVICE_TIMEOUT_S) {
      ESP_LOGI(kTag, "service mode idle for %lu s",
               static_cast<unsigned long>(idle));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  lanSessionEnd("service mode ended");
  lanServerClose();
  webPortalStop();
  dnsRedirectStop();
  d.wifi.stopAp();
  d.wifi.setKeepStation(false);
  ledBlink(0);
  d.sys.serviceMode = false;
  d.sys.exitService = false;
  ESP_LOGI(kTag, "service mode finished, back to the measurement cycle");
}

extern "C" void app_main(void) {
  // Local time in log timestamps (HH:MM:SS.sss); epochs stay UTC.
  setenv("TZ", CONFIG_GM_TIMEZONE, 1);
  tzset();
  gm::setLogSink(logSink);
  Device& d = device();
  d.lock = xSemaphoreCreateMutex();
  boardIoInit();

  const bool nvsOk = initNvs() == ESP_OK && d.store.open() == ESP_OK;
  const bool busOk = d.bus.init() == ESP_OK;
  const bool wifiOk = d.wifi.init() == ESP_OK;
  d.sys.init(&d.wifi);
  powerStatsBoot(!d.sys.coldBoot);
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
  ports.history = d.history.open() ? &d.history : nullptr;
  gm::ConfigLimits limits;
  limits.minMeasureS = 60;
  limits.allowInsecureUrl = false;  // ThingSpeak only over verified HTTPS
  d.app = new gm::GrainMeter(ports, &s_retained, limits,
                             "https://api.thingspeak.com",
                             CONFIG_GM_DEFAULT_UPDATE_URL);
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

  // After RST / power-on (not a deep sleep wake): join Wi-Fi at once, set
  // the clock and offer the LAN console for 3 s, so a waiting
  // tools/lan_console.py gets a session right after pressing RST.
  bool wifiSet;
  bool lanWake;
  {
    DeviceLock lock;
    wifiSet = d.app->config().wifiSsid[0] != '\0';
    lanWake = d.app->config().lanWake != 0;
  }
  const bool resume = lanTakeResume();
  if (d.sys.coldBoot && !resume && !service && wifiSet && lanWake &&
      batteryAllowsRadio()) {
    connectLan();
    if (d.wifi.connected()) {
      d.wifi.syncTime(gm::kTimeSyncTimeoutMs);
      if (lanOffer(lanAnnounce(), 3000) && lanWaitLogin()) {
        ESP_LOGI(kTag, "LAN console connected after reset: staying awake");
      }
    }
    if (!lanSessionActive()) {
      d.wifi.setKeepStation(false);
    }
  }
  d.wifi.setDropHook(lanWakeHook);

  if (resume && batteryAllowsRadio()) {
    // Restarted during a LAN console session: announce for up to 2 min.
    ESP_LOGI(kTag, "resuming the LAN console");
    connectLan();
    for (int i = 0; i < 40 && d.wifi.connected() && !lanSessionActive(); ++i) {
      if (lanOffer(lanAnnounce(), 3000)) lanWaitLogin();
    }
    if (!lanSessionActive()) {
      d.wifi.setKeepStation(false);
    }
  }

  const int64_t bootUs = esp_timer_get_time();
  // Bench: a computer on USB starts service mode (portal also on the local
  // network). Leaving it with exit-service holds until the cable is removed.
  bool benchDismissed = false;
  while (true) {
    const bool usbHost = usbConnected();
    if (!usbHost) {
      benchDismissed = false;
    }
    const bool wanted = service || d.sys.enterService ||
                        serviceWakeButtonPressed() ||
                        (usbHost && !benchDismissed);
    if (wanted || lanSessionActive()) {
      if (usbHost && !service && !d.sys.enterService) {
        ESP_LOGI(kTag, "computer on USB: service mode (bench)");
      }
      runServiceMode(wanted);
      service = false;
      benchDismissed = usbConnected();
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
    if (usbActive || d.sys.enterService || lanClientConnected()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    deepSleep(sleepS);
  }
}
