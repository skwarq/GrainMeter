// SPDX-License-Identifier: MIT
#pragma once

#include "app/platform.h"

// Monotonic time from the RTC timer (keeps counting in deep sleep, restarts
// on power loss). Wall clock = system time, which ESP-IDF keeps across deep
// sleep once SNTP set it.
class EspClock : public gm::Clock {
 public:
  uint32_t monoS() override;
  bool wallValid() override;
  uint32_t epoch() override;
};
