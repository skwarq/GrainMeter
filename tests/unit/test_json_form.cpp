// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "util/form.h"
#include "util/json.h"

TEST(Json, NestedStructureAndEscaping) {
  char buf[256];
  gm::JsonWriter w(buf, sizeof(buf));
  w.beginObject()
      .kv("a", "x\"y\\z\n")
      .key("b")
      .beginArray()
      .num(-1)
      .centi(-5)
      .centi(2153)
      .null()
      .endArray();
  w.kvb("c", true).key("d").beginObject().endObject().endObject();
  ASSERT_TRUE(w.ok());
  EXPECT_STREQ(buf,
               "{\"a\":\"x\\\"y\\\\z\\n\",\"b\":[-1,-0.05,21.53,null],\"c\":"
               "true,\"d\":{}}");
}

TEST(Json, OverflowIsSticky) {
  char buf[8];
  gm::JsonWriter w(buf, sizeof(buf));
  w.beginObject().kv("long-key", "value").endObject();
  EXPECT_FALSE(w.ok());
  EXPECT_LT(std::strlen(buf), sizeof(buf));
}

TEST(Form, DecodesFields) {
  const char* body = "a=1&name=Silo+%231&empty=&flag";
  gm::Form f(body, std::strlen(body));
  char v[32];
  ASSERT_TRUE(f.get("name", v, sizeof(v)));
  EXPECT_STREQ(v, "Silo #1");
  ASSERT_TRUE(f.get("empty", v, sizeof(v)));
  EXPECT_STREQ(v, "");
  EXPECT_TRUE(f.has("flag"));
  EXPECT_FALSE(f.has("nam"));
  long n = 0;
  EXPECT_TRUE(f.getInt("a", 0, 5, &n));
  EXPECT_EQ(n, 1);
  EXPECT_FALSE(f.getInt("a", 2, 5, &n));
}

TEST(Form, RejectsMalformedAndOverflow) {
  const char* body = "x=%4&y=%G1&z=abcdef";
  gm::Form f(body, std::strlen(body));
  char v[4];
  EXPECT_FALSE(f.get("x", v, sizeof(v)));
  EXPECT_FALSE(f.get("y", v, sizeof(v)));
  EXPECT_FALSE(f.get("z", v, sizeof(v)));  // longer than buffer
}

TEST(Secret, ConstantTimeCompare) {
  EXPECT_TRUE(gm::secretEquals("abc12345", "abc12345"));
  EXPECT_FALSE(gm::secretEquals("abc12345", "abc12346"));
  EXPECT_FALSE(gm::secretEquals("abc", "abc12345"));
  EXPECT_FALSE(gm::secretEquals("", ""));
}
