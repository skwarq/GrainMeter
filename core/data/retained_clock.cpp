// SPDX-License-Identifier: MIT
#include "data/retained_clock.h"

#include <stddef.h>

#include "util/crc.h"

namespace gm {
namespace {
const uint32_t kMagic = 0x474D4331u;  // "GMC1", layout v1
const uint64_t kMaxEpochUs = static_cast<uint64_t>(UINT32_MAX) * 1000000ULL;
}  // namespace

void RetainedClock::save(uint64_t wallUs, uint64_t monoUs) {
  // CRC is committed last; an interrupted write is rejected on boot.
  crc = 0;
  magic = kMagic;
  size = sizeof(RetainedClock);
  epochUs = wallUs;
  rtcUs = monoUs;
  reserved = 0;
  crc = crc32(this, offsetof(RetainedClock, crc));
}

bool RetainedClock::restore(ClockReset reset, uint64_t nowRtcUs,
                            uint64_t* wallUs) const {
  if (reset != ClockReset::Software && reset != ClockReset::Panic &&
      reset != ClockReset::Watchdog) {
    return false;
  }
  if (wallUs == nullptr || magic != kMagic || size != sizeof(RetainedClock) ||
      reserved != 0 || crc != crc32(this, offsetof(RetainedClock, crc)) ||
      epochUs < static_cast<uint64_t>(kMinValidEpoch) * 1000000ULL ||
      epochUs > kMaxEpochUs || nowRtcUs < rtcUs) {
    return false;
  }
  const uint64_t elapsed = nowRtcUs - rtcUs;
  if (elapsed > kClockSnapshotMaxAgeUs || elapsed > kMaxEpochUs - epochUs) {
    return false;
  }
  *wallUs = epochUs + elapsed;
  return true;
}

}  // namespace gm
