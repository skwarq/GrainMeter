// SPDX-License-Identifier: MIT
// Buffering and reporting through the shared application logic with the
// simulated probe, network and ThingSpeak stand-in (manual clock).
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "support/test_device.h"

namespace {
const uint32_t kMain = 1000001;  // simulator's provisioned channel

void setup(TestDevice& t) {
  ASSERT_TRUE(t.started);
  std::lock_guard<std::mutex> lock(t.dev->mutex());
  ASSERT_EQ(t.dev->app().mapAuto(), 8u);
}

std::set<std::string> uniqueTimestamps(
    const std::vector<gmpc::MockThingSpeak::Entry>& e) {
  std::set<std::string> s;
  for (const auto& x : e) s.insert(x.createdAt);
  return s;
}
}  // namespace

TEST(Upload, HourlyBatchOfFourSamples) {
  TestDevice t("hourly");
  setup(t);
  t.runFor(3600);
  const auto entries = t.dev->mock().entries(kMain);
  EXPECT_EQ(entries.size(), 4u);
  EXPECT_EQ(t.dev->app().state().count, 1u);  // 5th sample at t=3600 waits
  // Diagnostic channel: battery of every measurement in bulk (field1 only)
  // plus one full row (all fields) in the first window.
  const auto diag = t.dev->mock().entries(1000002);
  size_t full = 0, battery = 0;
  for (const auto& e : diag) {
    if (e.values.count("field3")) {
      ++full;
    } else if (e.values.count("field1")) {
      ++battery;
    }
  }
  EXPECT_EQ(full, 1u);
  EXPECT_EQ(battery, 4u);  // measurements before the window
  std::set<std::string> diagTimes;
  for (const auto& e : diag) diagTimes.insert(e.createdAt);
  EXPECT_EQ(diagTimes.size(), diag.size());       // unique created_at
  EXPECT_EQ(t.dev->app().state().battCount, 1u);  // t=3600 waits, like sample 5
  EXPECT_EQ(t.dev->app().state().last.diagResult, gm::DiagSent);
  EXPECT_TRUE(t.dev->app().state().last.ok);
}

TEST(Upload, FailedUploadKeepsSamplesRetryDeliversAll) {
  TestDevice t("retry");
  setup(t);
  t.dev->network().setUp(false);
  t.runFor(2 * 3600);
  const gm::RetainedState& st = t.dev->app().state();
  EXPECT_EQ(t.dev->mock().entries(kMain).size(), 0u);
  EXPECT_GE(st.count, 8u);
  EXPECT_FALSE(st.last.ok);
  EXPECT_GE(st.uploadFailCount, 2u);
  EXPECT_LE(st.uploadFailCount,
            3u);  // once per upload interval, not per sample
  const uint32_t measured = st.nextSeq - 1;

  t.dev->network().setUp(true);
  t.runFor(3600);
  const auto entries = t.dev->mock().entries(kMain);
  // everything measured before + during recovery hour, minus what waits
  EXPECT_EQ(entries.size() + st.count, static_cast<size_t>(st.nextSeq - 1));
  EXPECT_GE(entries.size(), measured);
  EXPECT_EQ(uniqueTimestamps(entries).size(), entries.size());
  EXPECT_TRUE(st.last.ok);
}

TEST(Upload, LostResponseDoesNotDuplicate) {
  TestDevice t("lost");
  setup(t);
  t.dev->http().setLostResponse(true);
  t.runFor(3600);
  // Server stored the 4 entries, device did not get the confirmation.
  EXPECT_EQ(t.dev->mock().entries(kMain).size(), 4u);
  EXPECT_GE(t.dev->app().state().count, 4u);
  EXPECT_FALSE(t.dev->app().state().last.ok);

  t.dev->http().setLostResponse(false);
  t.runFor(3600);
  const auto entries = t.dev->mock().entries(kMain);
  const gm::RetainedState& st = t.dev->app().state();
  EXPECT_EQ(entries.size() + st.count, static_cast<size_t>(st.nextSeq - 1));
  EXPECT_EQ(uniqueTimestamps(entries).size(), entries.size())
      << "duplicate created_at in channel";
}

TEST(Upload, MissingSensorIsNeverSentAsZero) {
  TestDevice t("missing");
  setup(t);
  t.dev->probe().setPresent(2, false);
  t.dev->probe().setTemperature(3, 0.0);  // genuine 0.00 degC
  t.runFor(3600);
  const auto entries = t.dev->mock().entries(kMain);
  ASSERT_EQ(entries.size(), 4u);
  int zeroFields = 0;
  for (const auto& e : entries) {
    EXPECT_EQ(e.values.size(), 8u);  // 7 fields + status
    for (const auto& kv : e.values) {
      if (kv.first != "status" && kv.second == "0.00") ++zeroFields;
    }
    EXPECT_NE(e.values.at("status").find(":missing"), std::string::npos);
  }
  EXPECT_EQ(zeroFields, 4);  // only the genuine 0.00 sensor
}

TEST(Upload, PowerLossReinitialisesBuffer) {
  TestDevice t("power");
  setup(t);
  t.dev->network().setUp(false);
  t.runFor(1800);
  EXPECT_GT(t.dev->app().state().count, 0u);
  char msg[128];
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    const char* body = "action=powerloss";
    ASSERT_TRUE(t.dev->platformAction("sim", gm::Form(body, std::strlen(body)),
                                      msg, sizeof(msg)));
  }
  EXPECT_EQ(t.dev->app().state().count, 0u);
  EXPECT_TRUE(t.dev->app().state().valid());
  // mapping (persistent store) survives the power loss
  EXPECT_EQ(t.dev->app().romMap().size(), 8u);
}

TEST(Upload, NotConfiguredDoesNotTouchNetwork) {
  TestDevice t("unconfigured");
  setup(t);
  gm::Form f("ts_channel=0", 12);
  char err[96];
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    ASSERT_TRUE(t.dev->app().applyConfig(f, err, sizeof(err)));
  }
  t.runFor(2 * 3600);
  EXPECT_EQ(t.dev->network().connectCount(), 0u);
  EXPECT_GE(t.dev->app().state().count, 8u);
}

TEST(Upload, HistoryBackfilledAfterFirstTimeSync) {
  TestDevice t("backfill");
  setup(t);
  t.runFor(2 * 900);  // 3 samples, before the first upload window
  gm::HistoryLog& h = t.dev->app().history();
  gm::HistoryRecord last;
  EXPECT_FALSE(h.last(&last));  // nothing without a clock
  t.runFor(3600);               // first upload window syncs the clock
  std::vector<gm::HistoryRecord> recs;
  ASSERT_TRUE(h.forEach(
      0, 0xFFFFFFFFu,
      [](void* ctx, const gm::HistoryRecord& r) {
        static_cast<std::vector<gm::HistoryRecord>*>(ctx)->push_back(r);
        return true;
      },
      &recs));
  // the measurements taken before the clock was known come first, marked
  // as a restart with reconstructed times; then live ones, in order
  ASSERT_GE(recs.size(), 2u);
  EXPECT_TRUE(recs[0].flags & gm::kHistRestart);
  EXPECT_TRUE(recs[0].flags & gm::kHistReconstructed);
  EXPECT_FALSE(recs.back().flags & gm::kHistReconstructed);
  EXPECT_NE(recs.back().batteryMv, 0);
  for (size_t i = 1; i < recs.size(); ++i)
    EXPECT_GT(recs[i].epoch, recs[i - 1].epoch);
  EXPECT_LE(recs.size(), static_cast<size_t>(t.dev->app().state().nextSeq));
}

TEST(Upload, HttpDateCorrectsClockDrift) {
  TestDevice t("datefix");
  setup(t);
  t.runFor(3600);  // first window: SNTP
  const unsigned sntp = t.dev->network().syncCount();
  EXPECT_EQ(sntp, 1u);
  t.dev->clock().setSkewS(-40);  // RC clock 40 s slow
  EXPECT_NE(t.dev->clock().epoch(), t.dev->clock().trueEpoch());
  t.runFor(3600);  // next window: corrected from the Date header
  EXPECT_EQ(t.dev->network().syncCount(), sntp);  // no extra SNTP
  const int64_t diff = static_cast<int64_t>(t.dev->clock().epoch()) -
                       static_cast<int64_t>(t.dev->clock().trueEpoch());
  EXPECT_LE(diff < 0 ? -diff : diff, 1);
  const gm::RetainedState& st = t.dev->app().state();
  EXPECT_EQ(st.timeSource, 2u);  // HTTP Date
  EXPECT_GE(st.lastClockFixS, 39);
  EXPECT_LE(st.lastClockFixS, 41);
}

TEST(Upload, SntpAgainAfterADayWithoutDate) {
  TestDevice t("nodate");
  setup(t);
  t.dev->http().setClock(nullptr);  // replies without a Date header
  t.runFor(3600);
  EXPECT_EQ(t.dev->network().syncCount(), 1u);
  t.runFor(20 * 3600);
  EXPECT_EQ(t.dev->network().syncCount(), 1u);  // within a day: none
  t.runFor(5 * 3600);
  EXPECT_EQ(t.dev->network().syncCount(), 2u);  // > 24 h: SNTP again
}
