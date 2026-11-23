// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <memory>

#include "app/scheduler.h"

class SchedulerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    st.reset(new gm::RetainedState());
    st->init();
    gm::configDefaults(&cfg, "https://api.thingspeak.com");
    cfg.channelId = 1;
    std::snprintf(cfg.writeKey, sizeof(cfg.writeKey), "ABCDEFGH12345678");
  }
  void measured(uint32_t at) {
    st->hasMeasured = 1;
    st->lastMeasureMonoS = at;
    gm::Sample s = {};
    st->push(s);
  }
  std::unique_ptr<gm::RetainedState> st;
  gm::Config cfg;
};

TEST_F(SchedulerTest, DefaultsAre15MinAnd1Hour) {
  EXPECT_EQ(cfg.measureIntervalS, 900u);
  EXPECT_EQ(cfg.uploadIntervalS, 3600u);
  EXPECT_EQ(cfg.samplesPerUpload(), 4u);
}

TEST_F(SchedulerTest, FirstWakeMeasures) {
  const gm::Plan p = gm::schedulePlan(*st, cfg, 5);
  EXPECT_TRUE(p.measureDue);
  EXPECT_FALSE(p.uploadDue);
}

TEST_F(SchedulerTest, SleepsUntilNextMeasurement) {
  measured(100);
  gm::Plan p = gm::schedulePlan(*st, cfg, 400);
  EXPECT_FALSE(p.measureDue);
  EXPECT_EQ(p.sleepS, 600u);
  p = gm::schedulePlan(*st, cfg, 999);  // woke 1 s early: tolerated
  EXPECT_TRUE(p.measureDue);
}

TEST_F(SchedulerTest, UploadAfterFourSamples) {
  for (int i = 0; i < 3; ++i) measured(900u * static_cast<uint32_t>(i));
  EXPECT_FALSE(gm::schedulePlan(*st, cfg, 1800).uploadDue);
  measured(2700);
  EXPECT_TRUE(gm::schedulePlan(*st, cfg, 2700).uploadDue);
}

TEST_F(SchedulerTest, FailedUploadRetriedOncePerUploadInterval) {
  for (int i = 0; i < 4; ++i) measured(900u * static_cast<uint32_t>(i));
  st->hasUploadAttempt = 1;
  st->lastUploadAttemptMonoS = 2700;  // failed, samples kept
  measured(3600);
  EXPECT_FALSE(gm::schedulePlan(*st, cfg, 3600).uploadDue);  // not every 15 min
  measured(6300);
  EXPECT_TRUE(gm::schedulePlan(*st, cfg, 6300).uploadDue);
}

TEST_F(SchedulerTest, NoUploadWithoutConfiguration) {
  cfg.channelId = 0;
  for (int i = 0; i < 8; ++i) measured(900u * static_cast<uint32_t>(i));
  EXPECT_FALSE(gm::schedulePlan(*st, cfg, 7200).uploadDue);
}

TEST_F(SchedulerTest, RtcRestartTreatedAsOverdue) {
  measured(5000);
  EXPECT_TRUE(gm::schedulePlan(*st, cfg, 10).measureDue);
}
