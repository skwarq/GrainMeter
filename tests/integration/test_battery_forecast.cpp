// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <string>

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
