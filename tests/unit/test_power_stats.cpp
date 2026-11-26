// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "platform/power_stats.h"

namespace {
int64_t timerUs = 0;
uint64_t rtcUs = 10000000;

void awake(uint32_t seconds) {
  timerUs += static_cast<int64_t>(seconds) * 1000000;
  rtcUs += static_cast<uint64_t>(seconds) * 1000000;
}

void wakeAfter(uint32_t seconds) {
  rtcUs += static_cast<uint64_t>(seconds) * 1000000;
  timerUs = 0;
  powerStatsBoot(true);
}
}  // namespace

int64_t esp_timer_get_time() { return timerUs; }
uint64_t esp_rtc_get_time_us() { return rtcUs; }

TEST(PowerStats, MeasuredTimesThresholdEarlyWakeAndLongSleep) {
  powerStatsBoot(false);
  awake(1);
  powerStatsSleep(3600);
  wakeAfter(900);
  EXPECT_EQ(powerStatsMahPerDayCenti(), -1);
  awake(2100);
  powerStatsWifiStart();
  awake(598);
  EXPECT_EQ(powerStatsMahPerDayCenti(), -1);
  char out[2048];
  gm::JsonWriter unknown(out, sizeof(out));
  unknown.beginObject();
  powerStatsWrite(unknown);
  unknown.endObject();
  ASSERT_TRUE(unknown.ok());
  EXPECT_NE(std::string(out).find("\"est_mah_per_day\":null"),
            std::string::npos);
  awake(1);
  EXPECT_EQ(powerStatsMahPerDayCenti(), 70981);
  powerStatsWifiEnd();
  EXPECT_EQ(powerStatsMahPerDayCenti(), 70981);
  gm::JsonWriter known(out, sizeof(out));
  known.beginObject();
  powerStatsWrite(known);
  known.endObject();
  ASSERT_TRUE(known.ok());
  EXPECT_NE(std::string(out).find("\"est_mah_per_day\":709.81"),
            std::string::npos);
  powerStatsSleep(2700);
  wakeAfter(2700);
  const double hours = 1.75;
  const double mah = (25.0 * 2101 + 90.0 * 599 + 0.04 * 3600) / 3600;
  EXPECT_EQ(powerStatsMahPerDayCenti(),
            static_cast<int32_t>(std::floor(mah / hours * 2400 + 0.5)));
  awake(1);
  powerStatsSleep(86400);
  wakeAfter(86400);
  gm::JsonWriter w(out, sizeof(out));
  w.beginObject();
  powerStatsWrite(w);
  w.endObject();
  ASSERT_TRUE(w.ok());
  EXPECT_NE(std::string(out).find("\"sleep_s_total\":90000"),
            std::string::npos);
  EXPECT_GT(powerStatsMahPerDayCenti(), 0);
}
