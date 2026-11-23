// SPDX-License-Identifier: MIT
#include "sim/sim_clock.h"

#include <ctime>
#include <thread>

namespace gmpc {

SimClock::SimClock(double speed, bool manual)
    : speed_(speed > 0 ? speed : 1.0),
      manual_(manual),
      synced_(false),
      offsetMs_(0),
      base_(std::chrono::steady_clock::now()),
      baseEpoch_(static_cast<uint32_t>(std::time(nullptr))) {}

uint64_t SimClock::nowLocked() {
  if (manual_) {
    return offsetMs_;
  }
  const auto real = std::chrono::steady_clock::now() - base_;
  const double ms =
      std::chrono::duration<double, std::milli>(real).count() * speed_;
  return offsetMs_ + static_cast<uint64_t>(ms);
}

uint64_t SimClock::monoMs() {
  std::lock_guard<std::mutex> lock(mu_);
  return nowLocked();
}

uint32_t SimClock::monoS() { return static_cast<uint32_t>(monoMs() / 1000u); }

bool SimClock::wallValid() {
  std::lock_guard<std::mutex> lock(mu_);
  return synced_;
}

uint32_t SimClock::epoch() {
  std::lock_guard<std::mutex> lock(mu_);
  return baseEpoch_ + static_cast<uint32_t>(nowLocked() / 1000u);
}

void SimClock::advanceMs(uint64_t ms) {
  std::lock_guard<std::mutex> lock(mu_);
  offsetMs_ += ms;
}

void SimClock::sleepMs(uint32_t ms) {
  double speed;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (manual_) {
      offsetMs_ += ms;
      return;
    }
    speed = speed_;
  }
  const auto realUs = static_cast<int64_t>(ms * 1000.0 / speed);
  if (realUs > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(realUs));
  }
}

void SimClock::setSpeed(double speed) {
  if (speed <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  offsetMs_ = nowLocked();
  base_ = std::chrono::steady_clock::now();
  speed_ = speed;
}

double SimClock::speed() {
  std::lock_guard<std::mutex> lock(mu_);
  return speed_;
}

void SimClock::setSynced(bool synced) {
  std::lock_guard<std::mutex> lock(mu_);
  synced_ = synced;
}

void SimClock::powerCycle() {
  std::lock_guard<std::mutex> lock(mu_);
  const uint32_t wall = baseEpoch_ + static_cast<uint32_t>(nowLocked() / 1000u);
  offsetMs_ = 0;
  base_ = std::chrono::steady_clock::now();
  baseEpoch_ = wall;  // real time keeps going, the RTC counter restarts
  synced_ = false;
}

}  // namespace gmpc
