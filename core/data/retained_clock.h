// SPDX-License-Identifier: MIT
#ifndef GM_RETAINED_CLOCK_H
#define GM_RETAINED_CLOCK_H

#include <stdint.h>

namespace gm {

static const uint32_t kMinValidEpoch = 1735689600u;  // 2025-01-01
static const uint64_t kClockSnapshotMaxAgeUs = 86400ULL * 1000000ULL;

enum class ClockReset { Software, Panic, Watchdog, PowerOn, Brownout, Other };

// Independent of RetainedState: a crash during a sample update must not
// invalidate the clock. Magic and size also reject a moved/old OTA layout.
struct RetainedClock {
  uint32_t magic;
  uint32_t size;
  uint64_t epochUs;
  uint64_t rtcUs;
  uint32_t reserved;
  uint32_t crc;

  void save(uint64_t wallUs, uint64_t monoUs);
  bool restore(ClockReset reset, uint64_t nowRtcUs, uint64_t* wallUs) const;
};

static_assert(sizeof(RetainedClock) == 32, "RTC clock layout changed");

}  // namespace gm

#endif
