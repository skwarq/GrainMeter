// SPDX-License-Identifier: MIT
#ifndef GM_PC_SIM_CLOCK_H
#define GM_PC_SIM_CLOCK_H

#include <chrono>
#include <cstdint>
#include <mutex>

#include "app/platform.h"

namespace gmpc {

// Simulated time. Runs `speed` times faster than real time (accelerated
// intervals), or only moves when advanced explicitly (manual mode, tests).
// The wall clock becomes valid after a simulated SNTP sync, like the ESP32.
class SimClock : public gm::Clock {
 public:
  SimClock(double speed, bool manual);

  uint32_t monoS() override;
  bool wallValid() override;
  uint32_t epoch() override;
  bool setEpoch(uint32_t epoch) override;
  // Real ("server") time: the device clock is epoch() = true + skew.
  uint32_t trueEpoch();
  // Simulated RC drift of the device wall clock (seconds, tests).
  void setSkewS(int32_t s);

  uint64_t monoMs();
  void advanceMs(uint64_t ms);
  // Waits `ms` of simulated time (really sleeps ms/speed in real mode).
  void sleepMs(uint32_t ms);
  void setSpeed(double speed);
  double speed();
  void setSynced(bool synced);
  // Simulated power loss: RTC counter and wall clock restart.
  void powerCycle();

 private:
  uint64_t nowLocked();

  std::mutex mu_;
  double speed_;
  bool manual_;
  bool synced_;
  uint64_t offsetMs_;
  std::chrono::steady_clock::time_point base_;
  uint32_t baseEpoch_;
  int64_t skewS_ = 0;
};

}  // namespace gmpc

#endif
