// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <string>

#include "support/json_check.h"
#include "support/test_device.h"
#include "util/json.h"

TEST(BatteryForecastStatus, UnknownWithoutClockIsSerialized) {
  TestDevice t("battery_forecast");
  ASSERT_TRUE(t.started);
  char buf[16384];
  gm::JsonWriter status(buf, sizeof(buf));
  t.dev->app().writeStatus(status);
  ASSERT_TRUE(status.ok());
  const std::string expected =
      "\"battery_forecast\":{\"state\":\"unknown\",\"days\":null,\"slope_mv_"
      "per_day\":0,\"samples\":0}";
  EXPECT_NE(std::string(buf).find(expected), std::string::npos);
  gm::JsonWriter diag(buf, sizeof(buf));
  t.dev->app().writeDiag(diag);
  ASSERT_TRUE(diag.ok());
  EXPECT_NE(std::string(buf).find(expected), std::string::npos);
}

TEST(BatteryRuntimeStatus, RuntimeWithoutVoltageHistoryAndDiagnosticUpload) {
  TestDevice t("battery_runtime");
  ASSERT_TRUE(t.started);
  char msg[96];
  const char action[] = "action=battery&value=3840";
  ASSERT_TRUE(t.dev->platformAction("sim", gm::Form(action, sizeof(action) - 1),
                                    msg, sizeof(msg)))
      << msg;
  char buf[16384];
  gm::JsonWriter unknown(buf, sizeof(buf));
  t.dev->app().writeStatus(unknown);
  ASSERT_TRUE(unknown.ok());
  EXPECT_TRUE(jsoncheck::error(buf).empty());
  EXPECT_NE(
      std::string(buf).find(
          "\"battery_runtime\":{\"mah_per_day\":null,\"capacity_mah\":2500,"
          "\"soc_pct\":50,\"days\":null}"),
      std::string::npos);
  t.dev->setBatteryMahPerDayCenti(350);
  gm::JsonWriter known(buf, sizeof(buf));
  t.dev->app().writeStatus(known);
  ASSERT_TRUE(known.ok());
  EXPECT_TRUE(jsoncheck::error(buf).empty());
  EXPECT_NE(
      std::string(buf).find(
          "\"battery_runtime\":{\"mah_per_day\":3.50,\"capacity_mah\":2500,"
          "\"soc_pct\":50,\"days\":357.14}"),
      std::string::npos);
  t.runFor(3600);
  const auto entries = t.dev->mock().entries(1000002);
  bool found = false;
  for (const auto& entry : entries) {
    const auto status = entry.values.find("status");
    if (status == entry.values.end()) continue;
    found = true;
    EXPECT_NE(status->second.find("capacity=2500 mah=3.50 rt=357.14"),
              std::string::npos);
    EXPECT_LE(status->second.size(), 255u);
  }
  EXPECT_TRUE(found);
  for (const auto& entry : t.dev->mock().entries(1000001)) {
    const auto status = entry.values.find("status");
    ASSERT_NE(status, entry.values.end());
    EXPECT_EQ(status->second.find("mah="), std::string::npos);
  }
}
