// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>

#include "app/alarm.h"
#include "support/test_device.h"

namespace {
gm::MeasureReport reading(int16_t centi, uint32_t epoch = 100000) {
  gm::MeasureReport m = {};
  m.count = 1;
  m.epoch = epoch;
  m.monoS = epoch - 90000;
  m.readings[0].point = 1;
  m.readings[0].status = gm::ReadStatus::Ok;
  m.readings[0].centi = centi;
  return m;
}
gm::Config config() {
  gm::Config c;
  gm::configDefaults(&c, "mock://thingspeak");
  return c;
}
void apply(TestDevice& t, const char* text) {
  char err[128];
  ASSERT_TRUE(t.dev->app().applyConfig(gm::Form(text, std::strlen(text)), err,
                                       sizeof(err)))
      << err;
}
}  // namespace
TEST(Alarm, ThresholdHysteresisAndInvalidSensor) {
  auto c = config();
  c.alarmTempCenti = 3000;
  gm::AlarmState a = {};
  gm::HistoryLog h;
  gm::alarmEvaluate(a, c, reading(3000), h, 3600);
  EXPECT_TRUE(a.active);
  EXPECT_EQ(a.reason, gm::AlarmTemp);
  EXPECT_EQ(a.count, 1);
  gm::alarmEvaluate(a, c, reading(2950), h, 3600);
  EXPECT_TRUE(a.active);
  auto missing = reading(0);
  missing.readings[0].status = gm::ReadStatus::Missing;
  gm::alarmEvaluate(a, c, missing, h, 3600);
  EXPECT_TRUE(a.active);
  gm::alarmEvaluate(a, c, reading(2900), h, 3600);
  EXPECT_FALSE(a.active);
  EXPECT_EQ(a.count, 2);
  EXPECT_EQ(a.events[1].type, gm::NotifyClear);
}
TEST(Alarm, IndependentHysteresisAndUnmappedIgnored) {
  auto c = config();
  c.alarmTempCenti = 3000;
  gm::AlarmState a = {};
  gm::HistoryLog h;
  auto m = reading(3100);
  m.count = 2;
  m.readings[1] = m.readings[0];
  m.readings[1].point = 2;
  gm::alarmEvaluate(a, c, m, h, 3600);
  m.readings[0].centi = 2900;
  m.readings[1].centi = 2950;
  gm::alarmEvaluate(a, c, m, h, 3600);
  EXPECT_TRUE(a.active);
  EXPECT_EQ(a.point, 2);
  m.readings[1].point = 0;
  gm::alarmEvaluate(a, c, m, h, 3600);
  EXPECT_FALSE(a.active);
}
TEST(Alarm, RiseUsesHistoryAndRejectsAbsentOrStaleBaseline) {
  TestDevice t("alarm-history");
  ASSERT_TRUE(t.started);
  auto c = config();
  c.alarmRiseCenti = 200;
  c.alarmRiseH = 1;
  gm::AlarmState a = {};
  auto& h = t.dev->app().history();
  gm::alarmEvaluate(a, c, reading(2300, 100000), h, 600);
  EXPECT_FALSE(a.active);
  gm::HistoryRecord row = {};
  row.epoch = 96300;
  row.centi[0] = 2000;
  row.status[0] = static_cast<uint8_t>(gm::ReadStatus::Ok);
  ASSERT_TRUE(h.append(row, 0));
  gm::alarmEvaluate(a, c, reading(2200, 100000), h, 600);
  EXPECT_TRUE(a.active);
  EXPECT_EQ(a.reason, gm::AlarmRise);
  EXPECT_EQ(a.riseCenti, 200);
  a = {};
  gm::alarmEvaluate(a, c, reading(2300, 101000), h, 600);
  EXPECT_FALSE(a.active);
  a = {};
  gm::alarmEvaluate(a, c, reading(2300, 95000), h, 600);
  EXPECT_FALSE(a.active);  // history newer than the target is not a baseline
}
TEST(Alarm, RepeatIntervalAndZero) {
  auto c = config();
  gm::AlarmState a = {};
  a.active = 1;
  a.hasWindow = 1;
  a.lastWindowMonoS = 100;
  EXPECT_FALSE(gm::alarmWindowDue(a, c, 10899));
  EXPECT_TRUE(gm::alarmWindowDue(a, c, 10900));
  c.alarmRepeatMin = 0;
  EXPECT_FALSE(gm::alarmWindowDue(a, c, 100000));
  a.pendingWindow = 1;
  EXPECT_TRUE(gm::alarmWindowDue(a, c, 100000));
}
TEST(Alarm, QueueDropsOldestAndSurvivesSeal) {
  gm::RetainedState s;
  s.init();
  for (unsigned i = 0; i < gm::kNotifySlots + 3; ++i) {
    gm::NotifyEvent e = {};
    e.epoch = i;
    gm::alarmEnqueue(s.alarm, e);
  }
  EXPECT_EQ(s.alarm.count, gm::kNotifySlots);
  EXPECT_EQ(s.alarm.events[0].epoch, 3u);
  EXPECT_EQ(s.alarm.dropped, 3u);
  s.seal();
  EXPECT_TRUE(s.valid());
}
TEST(Alarm, CriticalDefersImmediateWindowUntilRecovery) {
  TestDevice t("alarm-critical");
  ASSERT_TRUE(t.started);
  t.dev->app().mapAuto();
  apply(
      t,
      "alarm_temp_c=30&alarm_repeat_min=1&notify_ntfy_url=mock://notify/test");
  t.dev->probe().setTemperature(1, 40);
  char msg[128];
  ASSERT_TRUE(t.dev->platformAction(
      "sim", gm::Form("action=battery&value=3200", 25), msg, sizeof(msg)));
  t.dev->tick();
  EXPECT_TRUE(t.dev->app().state().alarm.active);
  EXPECT_TRUE(t.dev->app().state().alarm.pendingWindow);
  EXPECT_EQ(t.dev->network().connectCount(), 0u);
  ASSERT_TRUE(t.dev->platformAction(
      "sim", gm::Form("action=battery&value=3800", 25), msg, sizeof(msg)));
  t.dev->tick();
  EXPECT_GT(t.dev->network().connectCount(), 0u);
  EXPECT_FALSE(t.dev->app().state().alarm.pendingWindow);
}
TEST(Alarm, ConfigValidationPersistenceAndSecret) {
  TestDevice t("alarm-config");
  ASSERT_TRUE(t.started);
  apply(t,
        "alarm_temp_c=35.25&alarm_rise_c=2.50&alarm_rise_h=72&alarm_repeat_min="
        "1440&notify_ntfy_url=mock://notify/"
        "topic&notify_tg_token=12345%3Asecret_bot&notify_tg_chat=-42");
  const auto& c = t.dev->app().config();
  EXPECT_EQ(c.alarmTempCenti, 3525u);
  char json[4096];
  gm::JsonWriter w(json, sizeof(json));
  gm::configWriteJson(w, c);
  EXPECT_EQ(std::string(json).find("secret_bot"), std::string::npos);
  EXPECT_NE(std::string(json).find("\"notify_tg_token_set\":true"),
            std::string::npos);
  for (const char* field :
       {"alarm_temp_c=80.01", "alarm_temp_c=-1", "alarm_rise_c=20.01",
        "alarm_rise_h=0", "alarm_rise_h=73", "alarm_repeat_min=1441",
        "notify_ntfy_url=https://ntfy.sh",
        "notify_ntfy_url=https://ntfy.sh/a/b", "notify_tg_token=bad/token"}) {
    char err[128];
    EXPECT_FALSE(t.dev->app().applyConfig(gm::Form(field, std::strlen(field)),
                                          err, sizeof(err)))
        << field;
  }
  t.dev->restart("test");
  EXPECT_EQ(t.dev->app().config().alarmTempCenti, 3525u);
  EXPECT_EQ(t.dev->app().config().alarmRepeatMin, 1440u);
}

TEST(Alarm, FailedNotificationWaitsAndBatteryIsLimitedDaily) {
  TestDevice t("alarm-retry");
  ASSERT_TRUE(t.started);
  t.dev->app().mapAuto();
  apply(
      t,
      "notify_ntfy_url=mock://notify/silo&alarm_temp_c=30&alarm_repeat_min=1");
  t.dev->probe().setTemperature(1, 40);
  t.dev->http().setLostResponse(true);
  t.dev->tick();
  EXPECT_EQ(t.dev->app().state().alarm.count, 1);
  t.dev->http().setLostResponse(false);
  t.dev->app().upload();
  EXPECT_EQ(t.dev->app().state().alarm.count, 0);
  auto& a = t.dev->app().state().alarm;
  gm::NotifyEvent e = {};
  e.type = gm::NotifyBattery;
  e.centi = 3400;
  gm::alarmEnqueue(a, e);
  t.dev->app().upload();
  EXPECT_EQ(a.count, 0);
  gm::alarmEnqueue(a, e);
  t.dev->app().upload();
  EXPECT_EQ(a.count, 1);
  t.dev->clock().advanceMs(86400000);
  t.dev->app().upload();
  EXPECT_EQ(a.count, 0);
}

TEST(Alarm, RepeatedWindowsPreserveRegularSchedule) {
  TestDevice t("alarm-repeat");
  ASSERT_TRUE(t.started);
  t.dev->app().mapAuto();
  apply(
      t,
      "notify_ntfy_url=mock://notify/silo&alarm_temp_c=30&alarm_repeat_min=1");
  t.dev->probe().setTemperature(1, 40);
  t.dev->tick();
  const auto connections = t.dev->network().connectCount();
  EXPECT_FALSE(t.dev->app().state().hasUploadAttempt);
  t.dev->tick();
  EXPECT_EQ(t.dev->network().connectCount(), connections);
  t.dev->clock().advanceMs(60000);
  t.dev->tick();
  EXPECT_EQ(t.dev->network().connectCount(), connections + 1);
  EXPECT_FALSE(t.dev->app().state().hasUploadAttempt);
}

TEST(Alarm, ImmediateMockUploadContainsAlarmAndRecoveryNotification) {
  TestDevice t("alarm-flow");
  ASSERT_TRUE(t.started);
  t.dev->app().mapAuto();
  apply(
      t,
      "alarm_temp_c=30&alarm_repeat_min=0&notify_ntfy_url=mock://notify/silo");
  t.dev->probe().setTemperature(1, 40);
  t.dev->tick();
  const auto entries = t.dev->mock().entries(1000001);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_NE(entries[0].values.at("status").find("alarm=temp"),
            std::string::npos);
  char json[16384];
  gm::JsonWriter w(json, sizeof(json));
  t.dev->app().writeDiag(w);
  ASSERT_TRUE(w.ok());
  EXPECT_NE(std::string(json).find("alarm temperatury"), std::string::npos);
  EXPECT_NE(std::string(json).find("\"topic\":\"silo\""), std::string::npos);
  t.dev->probe().setTemperature(1, 28);
  t.dev->clock().advanceMs(900000);
  t.dev->tick();
  EXPECT_FALSE(t.dev->app().state().alarm.active);
  t.dev->app().upload();
  gm::JsonWriter recovered(json, sizeof(json));
  t.dev->app().writeDiag(recovered);
  ASSERT_TRUE(recovered.ok());
  EXPECT_NE(std::string(json).find("alarm ustąpił"), std::string::npos);
}
