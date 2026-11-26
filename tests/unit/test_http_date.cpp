// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "util/http_date.h"

TEST(HttpDate, ParsesImfFixdate) {
  uint32_t e = 0;
  ASSERT_TRUE(gm::parseHttpDate("Wed, 26 Nov 2026 10:17:05 GMT", &e));
  EXPECT_EQ(e, 1795688225u);
  ASSERT_TRUE(gm::parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT", &e) == false);
  ASSERT_TRUE(gm::parseHttpDate("Thu, 29 Feb 2024 23:59:59 GMT", &e));
  EXPECT_EQ(e, 1709251199u);
  ASSERT_TRUE(gm::parseHttpDate("Fri, 01 Jan 2027 00:00:00 GMT", &e));
  EXPECT_EQ(e, 1798761600u);
}

TEST(HttpDate, RejectsOtherFormats) {
  uint32_t e = 0;
  EXPECT_FALSE(gm::parseHttpDate(nullptr, &e));
  EXPECT_FALSE(gm::parseHttpDate("", &e));
  EXPECT_FALSE(gm::parseHttpDate("Wednesday, 26-Nov-26 10:17:05 GMT", &e));
  EXPECT_FALSE(gm::parseHttpDate("Wed Nov 26 10:17:05 2026", &e));
  EXPECT_FALSE(gm::parseHttpDate("Wed, 26 Xyz 2026 10:17:05 GMT", &e));
  EXPECT_FALSE(gm::parseHttpDate("Wed, 26 Nov 2026 25:17:05 GMT", &e));
  EXPECT_FALSE(gm::parseHttpDate("Wed, 26 Nov 2026 10:17:05 CET", &e));
}
