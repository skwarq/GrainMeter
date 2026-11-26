// SPDX-License-Identifier: MIT
#include "platform/power_stats.h"

#include "esp_attr.h"
#include "esp_rtc_time.h"
#include "esp_timer.h"

namespace {

struct Stats {
  uint32_t magic;
  uint32_t wakes;         // app starts since power-on
  uint32_t windows;       // wakes with a station link
  uint64_t awakeMsTotal;  // app time, all wakes
  uint64_t bootMsTotal;   // before app_main (timer wakes only)
  uint64_t wifiMsTotal;   // radio on for the station
  uint64_t sleepSTotal;   // requested deep sleep time
  uint32_t lastAwakeMs;
  uint32_t maxAwakeMs;
  uint32_t lastBootMs;
  uint32_t lastWifiMs;
  uint32_t lastConnectMs;
  uint32_t lastSntpMs;
  uint64_t sleepStartRtcUs;  // RTC time when the last sleep started
  uint32_t sleepUs;          // its requested length
};

const uint32_t kMagic = 0x50575231;  // "PWR1"
RTC_NOINIT_ATTR Stats s;

int64_t s_wifiStartUs = -1;
bool s_wifiThisWake = false;

}  // namespace

void powerStatsBoot(bool fromDeepSleep) {
  if (s.magic != kMagic) {
    s = Stats();
    s.magic = kMagic;
  }
  ++s.wakes;
  // App start in RTC time = now - time since app start (esp_timer).
  const uint64_t appStartRtc =
      esp_rtc_get_time_us() - static_cast<uint64_t>(esp_timer_get_time());
  const uint64_t expected = s.sleepStartRtcUs + s.sleepUs;
  if (fromDeepSleep && s.sleepStartRtcUs != 0 && appStartRtc > expected &&
      appStartRtc - expected < 5000000) {  // plausibility: < 5 s
    s.lastBootMs = static_cast<uint32_t>((appStartRtc - expected) / 1000);
    s.bootMsTotal += s.lastBootMs;
  }
  s.sleepStartRtcUs = 0;
}

void powerStatsWifiStart() {
  if (s_wifiStartUs < 0) s_wifiStartUs = esp_timer_get_time();
}

void powerStatsWifiConnected() {
  if (s_wifiStartUs >= 0) {
    s.lastConnectMs =
        static_cast<uint32_t>((esp_timer_get_time() - s_wifiStartUs) / 1000);
  }
}

void powerStatsWifiEnd() {
  if (s_wifiStartUs < 0) return;
  s.lastWifiMs =
      static_cast<uint32_t>((esp_timer_get_time() - s_wifiStartUs) / 1000);
  s.wifiMsTotal += s.lastWifiMs;
  if (!s_wifiThisWake) {
    s_wifiThisWake = true;
    ++s.windows;
  }
  s_wifiStartUs = -1;
}

void powerStatsSntp(uint32_t ms) { s.lastSntpMs = ms; }

uint32_t powerStatsSleep(uint32_t sleepS) {
  powerStatsWifiEnd();  // radio goes off with the chip
  const uint32_t awake = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  s.lastAwakeMs = awake;
  if (awake > s.maxAwakeMs) s.maxAwakeMs = awake;
  s.awakeMsTotal += awake;
  s.sleepSTotal += sleepS;
  s.sleepUs = sleepS * 1000000u;
  s.sleepStartRtcUs = esp_rtc_get_time_us();
  return awake;
}

uint32_t powerStatsLastBootMs() { return s.lastBootMs; }
uint32_t powerStatsLastWifiMs() { return s.lastWifiMs; }

void powerStatsWrite(gm::JsonWriter& w) {
  // Rough energy estimate with typical ESP32-C6 currents (to be replaced by
  // a measurement): CPU awake 25 mA, Wi-Fi on 90 mA, boot 20 mA, deep sleep
  // 0.04 mA (board + probe).
  const double awakeH = static_cast<double>(s.awakeMsTotal) / 3.6e6;
  const double wifiH = static_cast<double>(s.wifiMsTotal) / 3.6e6;
  const double bootH = static_cast<double>(s.bootMsTotal) / 3.6e6;
  const double sleepH = static_cast<double>(s.sleepSTotal) / 3600.0;
  const double totalH = awakeH + bootH + sleepH;
  const double mAh =
      (awakeH - wifiH) * 25.0 + wifiH * 90.0 + bootH * 20.0 + sleepH * 0.04;
  const int32_t perDayCenti =
      totalH > 0.01 ? static_cast<int32_t>(mAh / totalH * 24.0 * 100.0) : 0;
  const uint32_t avgAwake =
      s.wakes > 1 ? static_cast<uint32_t>(s.awakeMsTotal / (s.wakes - 1)) : 0;
  w.key("power_stats")
      .beginObject()
      .kvu("wakes", s.wakes)
      .kvu("wifi_windows", s.windows)
      .kvu("last_awake_ms", s.lastAwakeMs)
      .kvu("avg_awake_ms", avgAwake)
      .kvu("max_awake_ms", s.maxAwakeMs)
      .kvu("last_boot_ms", s.lastBootMs)
      .kvu("last_wifi_ms", s.lastWifiMs)
      .kvu("last_connect_ms", s.lastConnectMs)
      .kvu("last_sntp_ms", s.lastSntpMs)
      .kvu("awake_s_total", s.awakeMsTotal / 1000)
      .kvu("wifi_s_total", s.wifiMsTotal / 1000)
      .kvu("sleep_s_total", s.sleepSTotal);
  w.key("est_mah_per_day").centi(perDayCenti);
  w.endObject();
}
