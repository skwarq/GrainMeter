// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <string>

#include "support/json_check.h"
#include "util/json_read.h"
#include "util/log_buffer.h"

TEST(LogBuffer, FragmentsWrapLossAndFutureCursor) {
  gm::LogBuffer logs;
  char out[8192];
  logs.append("I (1) app: par", 14);
  ASSERT_TRUE(logs.read(0, out, sizeof(out)));
  EXPECT_STREQ(out, "{\"next\":0,\"lost\":0,\"lines\":[]}");
  logs.append("tial\n", 5);
  ASSERT_TRUE(logs.read(0, out, sizeof(out)));
  EXPECT_NE(std::string(out).find("partial"), std::string::npos);
  for (int i = 2; i <= 30; ++i) {
    const std::string line = "I (" + std::to_string(i) + ") app: line\n";
    logs.append(line.data(), line.size());
  }
  ASSERT_TRUE(logs.read(1, out, sizeof(out)));
  EXPECT_EQ(jsoncheck::error(out), "");
  EXPECT_NE(std::string(out).find("\"lost\":13"), std::string::npos);
  EXPECT_NE(std::string(out).find("I (15)"), std::string::npos);
  EXPECT_EQ(std::string(out).find("I (14)"), std::string::npos);
  EXPECT_NE(std::string(out).find("\"next\":30"), std::string::npos);
  ASSERT_TRUE(logs.read(30, out, sizeof(out)));
  EXPECT_STREQ(out, "{\"next\":30,\"lost\":0,\"lines\":[]}");
  ASSERT_TRUE(logs.read(999, out, sizeof(out)));
  EXPECT_STREQ(out, "{\"next\":30,\"lost\":0,\"lines\":[]}");
  logs.append("W (31) fresh\n", 13);
  ASSERT_TRUE(logs.read(30, out, sizeof(out)));
  EXPECT_NE(std::string(out).find("fresh"), std::string::npos);
}

TEST(LogBuffer, EscapedResponseLimitPaginationAndTruncation) {
  gm::LogBuffer logs;
  const std::string longLine(400, '\1');
  for (int i = 0; i < 16; ++i)
    logs.append(longLine.data(), longLine.size(), true);
  char out[9000];
  ASSERT_TRUE(logs.read(0, out, sizeof(out)));
  EXPECT_LT(strlen(out), 8192u);
  EXPECT_EQ(jsoncheck::error(out), "");
  uint32_t next = 0;
  ASSERT_TRUE(gm::jsonGetUint(out, "next", &next));
  EXPECT_GT(next, 0u);
  EXPECT_LT(next, 16u);
  ASSERT_TRUE(logs.read(next, out, sizeof(out)));
  EXPECT_EQ(jsoncheck::error(out), "");
  uint32_t later = 0;
  ASSERT_TRUE(gm::jsonGetUint(out, "next", &later));
  EXPECT_GT(later, next);
  EXPECT_NE(std::string(out).find("\"lost\":0"), std::string::npos);
  char tiny[96];
  EXPECT_FALSE(logs.read(0, tiny, sizeof(tiny)));
}

TEST(LogBuffer, ReallocationPreservesSequenceAndReportsExpiredLines) {
  gm::LogBuffer logs(42);
  char out[256];
  ASSERT_TRUE(logs.read(30, out, sizeof(out)));
  EXPECT_STREQ(out, "{\"next\":42,\"lost\":12,\"lines\":[]}");
  logs.append("new\n", 4);
  ASSERT_TRUE(logs.read(42, out, sizeof(out)));
  EXPECT_STREQ(out, "{\"next\":43,\"lost\":0,\"lines\":[\"new\"]}");
}

TEST(LogBuffer, Utf8TruncationKeepsOnlyCompleteCharacters) {
  gm::LogBuffer logs;
  char out[2048];
  for (size_t prefix : {252u, 253u, 254u}) {
    const std::string line = std::string(prefix, 'a') + "\xe2\x82\xac\n";
    logs.append(line.data(), line.size());
  }
  ASSERT_TRUE(logs.read(0, out, sizeof(out)));
  EXPECT_EQ(jsoncheck::error(out), "");
  const std::string json(out);
  const size_t euro = json.find("\xe2\x82\xac");
  ASSERT_NE(euro, std::string::npos);
  EXPECT_EQ(json.find('\xe2', euro + 3), std::string::npos);
}
