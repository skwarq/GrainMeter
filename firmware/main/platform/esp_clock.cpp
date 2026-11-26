// SPDX-License-Identifier: MIT
#include "platform/esp_clock.h"

#include <sys/time.h>

#include <ctime>

#include "data/retained_clock.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_system.h"

namespace {
RTC_NOINIT_ATTR gm::RetainedClock s_clock;
const char* kTag = "clock";

gm::ClockReset resetKind(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_SW:
      return gm::ClockReset::Software;
    case ESP_RST_PANIC:
      return gm::ClockReset::Panic;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return gm::ClockReset::Watchdog;
    case ESP_RST_POWERON:
      return gm::ClockReset::PowerOn;
    case ESP_RST_BROWNOUT:
      return gm::ClockReset::Brownout;
    default:
      return gm::ClockReset::Other;
  }
}
}  // namespace

void EspClock::begin() {
  restored_ = false;
  const gm::ClockReset reset = resetKind(esp_reset_reason());
  if (reset == gm::ClockReset::PowerOn || reset == gm::ClockReset::Brownout) {
    s_clock = {};
    const timeval zero = {};
    if (settimeofday(&zero, nullptr) != 0) {
      ESP_LOGE(kTag, "failed to invalidate clock after power reset");
    }
    return;
  }
  if (reset == gm::ClockReset::Other) return;  // native deep-sleep time
  const uint64_t rtcUs = esp_rtc_get_time_us();
  uint64_t wallUs = 0;
  if (!s_clock.restore(reset, rtcUs, &wallUs)) {
    ESP_LOGW(kTag, "RTC clock snapshot rejected (CRC/layout/age/counter)");
    s_clock = {};
    return;
  }
  timeval current = {};
  if (gettimeofday(&current, nullptr) != 0) {
    ESP_LOGE(kTag, "failed to read system clock during recovery");
    return;
  }
  const uint64_t currentUs =
      static_cast<uint64_t>(current.tv_sec) * 1000000ULL + current.tv_usec;
  // Do not move a valid native IDF wall clock backwards.
  if (!wallValid() || currentUs < wallUs) {
    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(wallUs / 1000000ULL);
    tv.tv_usec = static_cast<suseconds_t>(wallUs % 1000000ULL);
    if (settimeofday(&tv, nullptr) != 0) {
      ESP_LOGE(kTag, "failed to restore RTC clock");
      return;
    }
  }
  restored_ = true;
  ESP_LOGI(kTag, "RTC clock recovered; snapshot age %llu us, network sync due",
           static_cast<unsigned long long>(rtcUs - s_clock.rtcUs));
  checkpoint();
}

void EspClock::checkpoint() {
  if (!wallValid()) return;
  const uint64_t beforeRtcUs = esp_rtc_get_time_us();
  timeval tv = {};
  if (gettimeofday(&tv, nullptr) != 0) {
    ESP_LOGE(kTag, "failed to read clock for RTC snapshot");
    return;
  }
  const uint64_t rtcUs = esp_rtc_get_time_us();
  if (rtcUs < beforeRtcUs || rtcUs - beforeRtcUs > 1000000ULL) {
    ESP_LOGW(kTag, "RTC snapshot skipped (inconsistent clock capture)");
    return;
  }
  s_clock.save(static_cast<uint64_t>(tv.tv_sec) * 1000000ULL + tv.tv_usec,
               beforeRtcUs + (rtcUs - beforeRtcUs) / 2);
}

uint32_t EspClock::monoS() {
  return static_cast<uint32_t>(esp_rtc_get_time_us() / 1000000ULL);
}

bool EspClock::wallValid() {
  return time(nullptr) >= static_cast<time_t>(gm::kMinValidEpoch);
}

uint32_t EspClock::epoch() { return static_cast<uint32_t>(time(nullptr)); }

bool EspClock::setEpoch(uint32_t epoch) {
  // The Date header has 1 s resolution: aim for the middle of that second.
  timeval tv = {};
  tv.tv_sec = static_cast<time_t>(epoch);
  tv.tv_usec = 500000;
  if (settimeofday(&tv, nullptr) != 0) {
    ESP_LOGE(kTag, "settimeofday failed");
    return false;
  }
  checkpoint();
  return true;
}
