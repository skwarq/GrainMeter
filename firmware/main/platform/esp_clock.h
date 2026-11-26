// SPDX-License-Identifier: MIT
#pragma once

#include "app/platform.h"

// Monotonic time from the RTC timer (keeps counting in deep sleep, restarts
// on power loss). Wall clock = system time plus a checked RTC snapshot for
// software/panic/watchdog recovery. Calls are serialized with DeviceLock.
class EspClock : public gm::Clock {
 public:
  void begin();
  uint32_t monoS() override;
  bool wallValid() override;
  uint32_t epoch() override;
  bool setEpoch(uint32_t epoch) override;
  bool wallRestored() override { return restored_; }
  void checkpoint() override;

 private:
  bool restored_ = false;
};
