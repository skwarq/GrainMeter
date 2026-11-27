// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "comm/thingspeak.h"
#include "onewire/ds18b20.h"

TEST(ThingSpeak, RuntimeStatusFormatAndLimit) {
  char status[256];
  ASSERT_TRUE(gm::tsFormatDiagStatus("1.0", "timer", "normal", 5000, 350, 71429,
                                     3600, 900, true, true, status,
                                     sizeof(status)));
  EXPECT_STREQ(status,
               "fw=1.0 wake=timer power=normal capacity=5000 mah=3.50 rt=714.29"
               " up=3600 ms=900 lan=1 tb=1");
  gm::DiagValues d = {-1, -61, 8, 1, 3, 0, 2, 17, status};
  char out[2048];
  ASSERT_TRUE(
      gm::tsBuildDiag(d, "DIAGKEY000000001", 1700000000u, out, sizeof(out)));
  EXPECT_NE(std::string(out).find(status), std::string::npos);
  ASSERT_TRUE(gm::tsFormatDiagStatus("1.0", "timer", "normal", 2500, -1, -1,
                                     3600, 900, false, false, status,
                                     sizeof(status)));
  EXPECT_STREQ(status,
               "fw=1.0 wake=timer power=normal capacity=2500"
               " up=3600 ms=900 lan=0 tb=0");
  ASSERT_TRUE(gm::tsFormatDiagStatus("1.0", "timer", "normal", 2500, 1, 0, 3600,
                                     900, true, false, status, sizeof(status)));
  EXPECT_NE(std::string(status).find("mah=0.01 rt=0.00"), std::string::npos);
  EXPECT_FALSE(gm::tsFormatDiagStatus("1.0", "timer", "normal", 2500, 350, 100,
                                      3600, 900, true, true, status, 40));
  const std::string longVersion(256, 'a');
  EXPECT_FALSE(gm::tsFormatDiagStatus(longVersion.c_str(), "timer", "normal",
                                      2500, 350, 100, 3600, 900, true, true,
                                      status, sizeof(status)));
}

TEST(ThingSpeak, WakeStatusFitsAndEnforces255CharacterLimit) {
  char status[512];
  ASSERT_TRUE(gm::tsFormatDiagStatus("0.4.2", "timer", "economy", UINT32_MAX,
                                     INT32_MAX, INT32_MAX, UINT32_MAX,
                                     UINT32_MAX, true, true, status, 256));
  EXPECT_LT(std::strlen(status), 256u);
  EXPECT_NE(std::string(status).find("up=4294967295 ms=4294967295 lan=1 tb=1"),
            std::string::npos);

  const size_t prefixLength = std::strlen(status) - std::strlen("0.4.2");
  const std::string version(255 - prefixLength, 'a');
  ASSERT_TRUE(gm::tsFormatDiagStatus(
      version.c_str(), "timer", "economy", UINT32_MAX, INT32_MAX, INT32_MAX,
      UINT32_MAX, UINT32_MAX, true, true, status, sizeof(status)));
  EXPECT_EQ(std::strlen(status), 255u);
  const std::string tooLong = version + 'a';
  EXPECT_FALSE(gm::tsFormatDiagStatus(
      tooLong.c_str(), "timer", "economy", UINT32_MAX, INT32_MAX, INT32_MAX,
      UINT32_MAX, UINT32_MAX, true, true, status, sizeof(status)));
}
namespace {
std::string fmt(uint32_t e) {
  char b[40];
  gm::tsFormatTime(e, b, sizeof(b));
  return b;
}

gm::Sample sample(uint32_t epoch) {
  gm::Sample s;
  std::memset(&s, 0, sizeof(s));
  s.epoch = epoch;
  for (int p = 0; p < gm::kMaxPoints; ++p) {
    s.centi[p] = static_cast<int16_t>(1000 + p);
    s.status[p] = static_cast<uint8_t>(gm::ReadStatus::Ok);
  }
  return s;
}
}  // namespace

TEST(ThingSpeak, TimeFormat) {
  EXPECT_EQ(fmt(0), "1970-01-01 00:00:00 +0000");
  EXPECT_EQ(fmt(951782400), "2000-02-29 00:00:00 +0000");
  EXPECT_EQ(fmt(1795467374), "2026-11-23 20:56:14 +0000");
  EXPECT_EQ(fmt(4102444799u), "2099-12-31 23:59:59 +0000");
}

TEST(ThingSpeak, SampleEpochReconstruction) {
  gm::Sample s = sample(0);
  s.monoS = 1000;
  EXPECT_EQ(gm::tsSampleEpoch(s, 1700003600u, 4600),
            1700000000u);                                 // 3600 s ago
  EXPECT_EQ(gm::tsSampleEpoch(s, 1700003600u, 500), 0u);  // RTC restarted
  s.epoch = 1600000000u;
  EXPECT_EQ(gm::tsSampleEpoch(s, 1700003600u, 500), 1600000000u);
}

class BulkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    st.reset(new gm::RetainedState());
    st->init();
  }
  std::string build(size_t max = 96) {
    static char out[24576];
    EXPECT_TRUE(gm::tsBuildBulk(*st, max, "KEY123456789ABCD", 1700100000u,
                                100000, out, sizeof(out), &info));
    return out;
  }
  std::unique_ptr<gm::RetainedState> st;
  gm::BulkInfo info{};
};

TEST_F(BulkTest, MissingValuesAreOmittedNeverZero) {
  gm::Sample s = sample(1700000000u);
  s.status[2] = static_cast<uint8_t>(gm::ReadStatus::CrcError);
  s.status[4] = static_cast<uint8_t>(gm::ReadStatus::Missing);
  s.status[7] = static_cast<uint8_t>(gm::ReadStatus::NotMapped);
  s.centi[5] = 0;  // genuine 0.00 degC
  st->push(s);
  const std::string j = build();
  EXPECT_NE(j.find("\"write_api_key\":\"KEY123456789ABCD\""),
            std::string::npos);
  EXPECT_NE(j.find("\"created_at\":\"2023-11-14 22:13:20 +0000\""),
            std::string::npos);
  EXPECT_NE(j.find("\"field1\":10.00"), std::string::npos);
  EXPECT_EQ(j.find("\"field3\""), std::string::npos);
  EXPECT_EQ(j.find("\"field5\""), std::string::npos);
  EXPECT_EQ(j.find("\"field8\""), std::string::npos);
  EXPECT_NE(j.find("\"field6\":0.00"), std::string::npos);
  EXPECT_NE(j.find("\"status\":\"seq=1 p3:crc_error p5:missing\""),
            std::string::npos);
}

TEST_F(BulkTest, TimestampsUniqueAndBatchLimited) {
  for (int i = 0; i < 5; ++i) st->push(sample(1700000000u));  // same second
  const std::string j = build(3);
  EXPECT_EQ(info.entries, 3u);
  EXPECT_EQ(info.firstSeq, 1u);
  EXPECT_EQ(info.lastSeq, 3u);
  EXPECT_NE(j.find("22:13:20"), std::string::npos);
  EXPECT_NE(j.find("22:13:21"), std::string::npos);
  EXPECT_NE(j.find("22:13:22"), std::string::npos);
}

TEST_F(BulkTest, FullBufferFitsOneRequest) {
  for (int i = 0; i < gm::kBufferCapacity; ++i)
    st->push(sample(1700000000u + 900u * static_cast<uint32_t>(i)));
  build();
  EXPECT_EQ(info.entries, static_cast<size_t>(gm::kBufferCapacity));
}

TEST_F(BulkTest, SmallBufferSendsPrefix) {
  for (int i = 0; i < 10; ++i)
    st->push(sample(1700000000u + 900u * static_cast<uint32_t>(i)));
  char out[900];
  ASSERT_TRUE(gm::tsBuildBulk(*st, 96, "KEY123456789ABCD", 1700100000u, 100000,
                              out, sizeof(out), &info));
  EXPECT_GT(info.entries, 0u);
  EXPECT_LT(info.entries, 10u);
  EXPECT_EQ(out[std::strlen(out) - 1], '}');
}

TEST(ThingSpeak, ResponseClassification) {
  EXPECT_EQ(gm::tsParseResponse(true, 200, "{\"success\":true}"),
            gm::TsOutcome::Confirmed);
  EXPECT_EQ(gm::tsParseResponse(true, 200, "{ \"success\" : true }"),
            gm::TsOutcome::Confirmed);
  EXPECT_EQ(gm::tsParseResponse(true, 200, "{\"success\":false}"),
            gm::TsOutcome::RetryLater);
  EXPECT_EQ(gm::tsParseResponse(true, 200, "0"), gm::TsOutcome::RetryLater);
  // api.thingspeak.com answers bulk writes with 202 Accepted
  EXPECT_EQ(gm::tsParseResponse(true, 202, "{\"success\":true}"),
            gm::TsOutcome::Confirmed);
  EXPECT_EQ(gm::tsParseResponse(true, 202, "{\"success\":false}"),
            gm::TsOutcome::RetryLater);
  EXPECT_EQ(gm::tsParseResponse(true, 201, "{\"success\":true}"),
            gm::TsOutcome::Rejected);
  EXPECT_EQ(gm::tsParseResponse(true, 429, ""), gm::TsOutcome::RetryLater);
  EXPECT_EQ(gm::tsParseResponse(true, 503, ""), gm::TsOutcome::RetryLater);
  EXPECT_EQ(gm::tsParseResponse(true, 401, ""), gm::TsOutcome::Rejected);
  EXPECT_EQ(gm::tsParseResponse(false, -1, ""), gm::TsOutcome::RetryLater);
}

TEST(ThingSpeak, DiagMessage) {
  gm::DiagValues d = {-1, -61, 8, 1, 3, 0, 2, 17, "fw=1.0"};
  char out[512];
  ASSERT_TRUE(
      gm::tsBuildDiag(d, "DIAGKEY000000001", 1700000000u, out, sizeof(out)));
  const std::string j = out;
  EXPECT_EQ(j.find("\"field1\""), std::string::npos);  // battery n/a
  EXPECT_NE(j.find("\"field2\":-61"), std::string::npos);
  EXPECT_NE(j.find("\"field8\":17"), std::string::npos);
}

TEST(ThingSpeak, DiagBulkBatteryPointsAndFullRow) {
  static gm::RetainedState st;
  st.init();
  gm::BatteryPoint p = {};
  p.epoch = 1700000000u;
  p.mv = 3900;
  st.battPush(p);
  p.epoch = 0;  // measured before the clock was known: reconstructed
  p.monoS = 900;
  p.mv = 3890;
  st.battPush(p);
  gm::DiagValues d = {};
  d.batteryMv = 3880;
  d.status = "fw=x";
  char out[2048];
  size_t points = 0;
  ASSERT_TRUE(gm::tsBuildDiagBulk(st, &d, "DIAGKEY000000001", 1700003600u, 1800,
                                  out, sizeof(out), &points));
  EXPECT_EQ(points, 2u);
  const std::string body(out);
  EXPECT_NE(body.find("\"field1\":3900"), std::string::npos) << body;
  // 1700003600 - (1800 - 900) = 1700002700 -> 2023-11-14 23:58:20 UTC
  EXPECT_NE(body.find("\"field1\":3890"), std::string::npos) << body;
  EXPECT_NE(body.find("\"field3\""), std::string::npos);  // full row
  ASSERT_TRUE(gm::tsBuildDiagBulk(st, nullptr, "DIAGKEY000000001", 1700003600u,
                                  1800, out, sizeof(out), &points));
  EXPECT_EQ(std::string(out).find("\"field3\""), std::string::npos);
  st.battRemoveFront(points);
  EXPECT_EQ(st.battCount, 0u);
}
