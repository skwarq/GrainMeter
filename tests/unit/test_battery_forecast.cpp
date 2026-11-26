// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <vector>

#include "app/battery_forecast.h"
namespace {
TEST(BatteryRuntime, SocCurveAndInterpolation) {
  EXPECT_EQ(gm::batteryPercent(-1), -1);
  EXPECT_EQ(gm::batteryPercent(0), -1);
  EXPECT_EQ(gm::batteryPercent(2900), 0);
  EXPECT_EQ(gm::batteryPercent(4350), 100);
  EXPECT_EQ(gm::batteryPercent(3840), 50);
  EXPECT_EQ(gm::batteryPercent(3650), 8);
  EXPECT_EQ(gm::batteryPercent(3500), 4);
  EXPECT_EQ(gm::batteryPercent(3930), 68);
  int previous = 0;
  for (int mv = 3000; mv <= 4200; ++mv) {
    const int pct = gm::batteryPercent(mv);
    EXPECT_GE(pct, previous);
    EXPECT_LE(pct, 100);
    previous = pct;
  }
}

TEST(BatteryRuntime, ConsumptionAndMissingData) {
  EXPECT_EQ(gm::batteryRuntimeCentiDays(3840, 2500, 350), 35714);
  EXPECT_EQ(gm::batteryRuntimeCentiDays(3840, 5000, 350), 71429);
  EXPECT_EQ(gm::batteryRuntimeCentiDays(3000, 2500, 350), 0);
  EXPECT_EQ(gm::batteryRuntimeCentiDays(4200, 20000, 1), 200000000);
  EXPECT_EQ(gm::batteryRuntimeCentiDays(-1, 2500, 350), -1);
  EXPECT_EQ(gm::batteryRuntimeCentiDays(3840, 2500, -1), -1);
  EXPECT_EQ(gm::batteryRuntimeCentiDays(3840, 2500, 0), -1);
  EXPECT_EQ(gm::batteryRuntimeCentiDays(3840, 99, 350), -1);
  EXPECT_EQ(gm::batteryRuntimeCentiDays(3840, 20001, 350), -1);
}

const uint32_t base = 1700000000;
std::vector<gm::BatterySample> series(int hours, int drop = 1) {
  std::vector<gm::BatterySample> p;
  for (int h = 0; h <= hours; ++h)
    p.push_back({base + static_cast<uint32_t>(h) * 3600,
                 static_cast<uint32_t>(3900 - h * drop)});
  return p;
}
gm::BatteryForecast forecast(const std::vector<gm::BatterySample>& p) {
  return gm::batteryForecast(p.data(), p.size(), p.back().epoch, 3300);
}
TEST(BatteryForecast, LinearAndLimits) {
  auto p = series(48);
  auto f = forecast(p);
  EXPECT_EQ(f.state, gm::BatteryForecast::Discharging);
  EXPECT_EQ(f.slopeMvPerDay, -24);
  EXPECT_EQ(f.days, 23);
  EXPECT_EQ(f.samples, 49u);
  EXPECT_EQ(gm::batteryForecast(p.data(), p.size(), p.back().epoch, 4000).days,
            0);
  EXPECT_EQ(gm::batteryForecast(p.data(), p.size(), p.back().epoch, 1).days,
            160);
  p = series(48, 0);
  for (size_t i = 0; i < p.size(); ++i)
    p[i].batteryMv = 3900 - static_cast<uint32_t>(i / 4);
  EXPECT_EQ(gm::batteryForecast(p.data(), p.size(), p.back().epoch, 1).days,
            365);
  p = series(24, 0);
  for (size_t i = 0; i < p.size(); ++i)
    p[i].batteryMv = 3000 + static_cast<uint32_t>(i) * 25;
  EXPECT_EQ(forecast(p).state, gm::BatteryForecast::Charging);
}
TEST(BatteryForecast, SolarRejectsChargingOffsets) {
  auto p = series(72);
  int mv = 3000;
  for (size_t i = 0; i < p.size(); ++i) {
    if (i) mv += i % 24 < 12 ? -2 : 30;
    p[i].batteryMv = static_cast<uint32_t>(mv);
  }
  EXPECT_EQ(forecast(p).state, gm::BatteryForecast::Discharging);
  EXPECT_EQ(forecast(p).slopeMvPerDay, -48);
}
TEST(BatteryForecast, GapsInsufficientStableAndCharging) {
  auto p = series(48, 0);
  EXPECT_EQ(forecast(p).state, gm::BatteryForecast::Stable);
  EXPECT_EQ(forecast(p).days, -1);
  p = series(12);
  EXPECT_EQ(forecast(p).state, gm::BatteryForecast::Unknown);
  p = {{base, 3900},
       {base + 86400, 3800},
       {base + 172800, 3700},
       {base + 259200, 3600}};
  EXPECT_EQ(forecast(p).state, gm::BatteryForecast::Unknown);
  p = series(72);
  p.erase(p.begin() + 10, p.begin() + 24);
  EXPECT_EQ(forecast(p).slopeMvPerDay, -24);
  p = series(24, 0);
  for (auto& v : p) v.batteryMv = 4200;
  EXPECT_EQ(forecast(p).state, gm::BatteryForecast::Charging);
  p = series(48);
  EXPECT_EQ(
      gm::batteryForecast(p.data(), p.size(), p.back().epoch + 86400, 3300)
          .state,
      gm::BatteryForecast::Unknown);
  EXPECT_EQ(gm::batteryForecast(nullptr, 0, base, 3300).state,
            gm::BatteryForecast::Unknown);
}
}  // namespace
