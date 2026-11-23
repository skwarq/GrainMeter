// SPDX-License-Identifier: MIT
#include "platform/esp_clock.h"

#include <ctime>

#include "esp_rtc_time.h"

static const time_t kMinValidEpoch = 1735689600;  // 2025-01-01

uint32_t EspClock::monoS() {
  return static_cast<uint32_t>(esp_rtc_get_time_us() / 1000000ULL);
}

bool EspClock::wallValid() { return time(nullptr) >= kMinValidEpoch; }

uint32_t EspClock::epoch() { return static_cast<uint32_t>(time(nullptr)); }
