// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <stddef.h>

#include "data/retained_clock.h"
#include "util/crc.h"

namespace {
const uint64_t kEpochUs = 1800000000500000ULL;
const uint64_t kRtcUs = 5000000250000ULL;
}  // namespace

TEST(RetainedClock, FreshSnapshotSurvivesSoftwarePanicAndWatchdog) {
  gm::RetainedClock saved = {};
  saved.save(kEpochUs, kRtcUs);
  for (auto reason : {gm::ClockReset::Software, gm::ClockReset::Panic,
                      gm::ClockReset::Watchdog}) {
    uint64_t restored = 0;
    ASSERT_TRUE(saved.restore(reason, kRtcUs + 1750000, &restored));
    EXPECT_EQ(restored, kEpochUs + 1750000);
  }
}

TEST(RetainedClock, RejectsPowerResetAndUnsupportedResetEvenWithValidCrc) {
  gm::RetainedClock saved = {};
  saved.save(kEpochUs, kRtcUs);
  for (auto reason : {gm::ClockReset::PowerOn, gm::ClockReset::Brownout,
                      gm::ClockReset::Other}) {
    uint64_t restored = 123;
    EXPECT_FALSE(saved.restore(reason, kRtcUs + 1, &restored));
    EXPECT_EQ(restored, 123u);
  }
}

TEST(RetainedClock, RejectsCorruptionOldLayoutAndInterruptedWrite) {
  gm::RetainedClock saved = {};
  saved.save(kEpochUs, kRtcUs);
  ++saved.epochUs;
  uint64_t restored = 0;
  EXPECT_FALSE(saved.restore(gm::ClockReset::Software, kRtcUs, &restored));
  saved.save(kEpochUs, kRtcUs);
  --saved.size;
  saved.crc = gm::crc32(&saved, offsetof(gm::RetainedClock, crc));
  EXPECT_FALSE(saved.restore(gm::ClockReset::Software, kRtcUs, &restored));
  saved.save(kEpochUs, kRtcUs);
  saved.crc = 0;
  EXPECT_FALSE(saved.restore(gm::ClockReset::Software, kRtcUs, &restored));
}

TEST(RetainedClock, RejectsExpiredSnapshotAndCounterRegression) {
  gm::RetainedClock saved = {};
  saved.save(kEpochUs, kRtcUs);
  uint64_t restored = 0;
  EXPECT_TRUE(saved.restore(gm::ClockReset::Software,
                            kRtcUs + gm::kClockSnapshotMaxAgeUs, &restored));
  EXPECT_FALSE(saved.restore(gm::ClockReset::Software,
                             kRtcUs + gm::kClockSnapshotMaxAgeUs + 1,
                             &restored));
  EXPECT_FALSE(saved.restore(gm::ClockReset::Watchdog, kRtcUs - 1, &restored));
}

TEST(RetainedClock, RestoredTimeIsMonotonicAcrossRepeatedRestarts) {
  gm::RetainedClock saved = {};
  saved.save(kEpochUs, kRtcUs);
  uint64_t first = 0, second = 0;
  ASSERT_TRUE(
      saved.restore(gm::ClockReset::Software, kRtcUs + 3000000, &first));
  saved.save(first, kRtcUs + 3000000);
  ASSERT_TRUE(saved.restore(gm::ClockReset::Panic, kRtcUs + 5500000, &second));
  EXPECT_EQ(second, kEpochUs + 5500000);
  EXPECT_GE(second, first);
}

TEST(RetainedClock, RejectsInvalidEpochAndEpochOverflow) {
  gm::RetainedClock saved = {};
  uint64_t restored = 0;
  saved.save(static_cast<uint64_t>(gm::kMinValidEpoch) * 1000000ULL - 1,
             kRtcUs);
  EXPECT_FALSE(saved.restore(gm::ClockReset::Software, kRtcUs, &restored));
  saved.save(static_cast<uint64_t>(UINT32_MAX) * 1000000ULL, kRtcUs);
  EXPECT_FALSE(saved.restore(gm::ClockReset::Software, kRtcUs + 1, &restored));
}
