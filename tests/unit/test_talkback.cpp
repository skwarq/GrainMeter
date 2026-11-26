// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "comm/talkback.h"
#include "support/test_device.h"
#include "util/json_read.h"

TEST(Talkback, StrictCommandAndExpiry) {
  gm::RemoteCommand c = {};
  ASSERT_TRUE(gm::talkbackParseCommand(" \tREMOTE 60 4294967295\r\n", &c));
  EXPECT_EQ(c.minutes, 60u);
  EXPECT_FALSE(gm::talkbackExpired(c, false, UINT32_MAX));
  EXPECT_TRUE(gm::talkbackExpired(c, true, UINT32_MAX));
  for (const char* s :
       {"", "REM", "REMOTE", "REMOTE 0 0", "REMOTE 61 0", "REMOTE -1 0",
        "REMOTE 1 -1", "REMOTE 1 4294967296", "REMOTE 1 0 junk", "REMOTE 1 1.0",
        "REMOTE 1", "OTHER 1 0", "REMOTE1 0"}) {
    EXPECT_FALSE(gm::talkbackParseCommand(s, &c)) << s;
  }
  ASSERT_TRUE(gm::talkbackParseCommand("REMOTE 1 0", &c));
  EXPECT_FALSE(gm::talkbackExpired(c, true, UINT32_MAX));
  c.validUntil = 100;
  EXPECT_FALSE(gm::talkbackExpired(c, true, 99));
  EXPECT_TRUE(gm::talkbackExpired(c, true, 100));
}

static bool config(TestDevice& t, const char* text) {
  char err[128];
  return t.dev->app().applyConfig(gm::Form(text, strlen(text)), err,
                                  sizeof(err));
}

TEST(Talkback, BudgetClippingDailyResetAndSeconds) {
  TestDevice t("remote_budget", 0);
  ASSERT_TRUE(
      config(t, "talkback_id=1&talkback_key=secret&remote_budget_min=3"));
  t.dev->http().queueTalkback("REMOTE 5 0");
  t.dev->app().upload();
  t.dev->app().remoteSessionUsed(61);
  uint32_t minutes = 99;
  ASSERT_TRUE(t.dev->app().takeRemoteRequest(&minutes));
  EXPECT_EQ(minutes, 1u);
  EXPECT_FALSE(t.dev->app().takeRemoteRequest(&minutes));
  EXPECT_EQ(minutes, 0u);
  t.dev->app().remoteSessionUsed(119);
  t.dev->http().queueTalkback("REMOTE 5 0");
  t.dev->app().upload();
  EXPECT_FALSE(t.dev->app().takeRemoteRequest(&minutes));
  t.dev->clock().advanceMs(86400000);
  t.dev->http().queueTalkback("REMOTE 5 0");
  t.dev->app().upload();
  ASSERT_TRUE(t.dev->app().takeRemoteRequest(&minutes));
  EXPECT_EQ(minutes, 3u);
  EXPECT_TRUE(t.dev->app().state().valid());
}

TEST(Talkback, UnknownClockUsageSurvivesTimeSyncAndRollingDay) {
  TestDevice t("remote_unknown", 0);
  t.dev->clock().setSynced(false);
  t.dev->app().remoteSessionUsed(120);
  EXPECT_EQ(t.dev->app().state().remoteUsedS, 120u);
  t.dev->clock().advanceMs(86400000);
  char status[20000];
  gm::JsonWriter w(status, sizeof(status));
  t.dev->app().writeStatus(w);
  EXPECT_EQ(t.dev->app().state().remoteUsedS, 0u);
  t.dev->app().remoteSessionUsed(60);
  t.dev->clock().setSynced(true);
  char diag[24000];
  gm::JsonWriter d(diag, sizeof(diag));
  t.dev->app().writeDiag(d);
  EXPECT_EQ(t.dev->app().state().remoteUsedS, 60u);
  EXPECT_TRUE(t.dev->app().state().remoteBudgetUtc);
}

TEST(Talkback, ExpiryZeroAndCritical) {
  TestDevice t("remote_block", 0);
  ASSERT_TRUE(config(t, "talkback_id=1&talkback_key=secret"));
  t.dev->http().queueTalkback("REMOTE 5 1");
  t.dev->app().upload();
  uint32_t m = 99;
  EXPECT_FALSE(t.dev->app().takeRemoteRequest(&m));
  t.dev->http().queueTalkback("REMOTE 5 0");
  t.dev->app().upload();
  ASSERT_TRUE(config(t, "remote_budget_min=0"));
  EXPECT_FALSE(t.dev->app().takeRemoteRequest(&m));
  ASSERT_TRUE(config(t, "remote_budget_min=60"));
  t.dev->http().queueTalkback("REMOTE 5 0");
  t.dev->app().upload();
  char msg[80];
  const char* action = "action=battery&value=3200";
  ASSERT_TRUE(t.dev->platformAction("sim", gm::Form(action, strlen(action)),
                                    msg, sizeof(msg)));
  EXPECT_FALSE(t.dev->app().takeRemoteRequest(&m));
  unsigned before = t.dev->http().talkbackCount();
  t.dev->app().upload();
  EXPECT_EQ(t.dev->http().talkbackCount(), before);
}

TEST(Talkback, RequestAndUnknownCommandDoNotExposeKey) {
  TestDevice t("remote_secret", 0);
  ASSERT_TRUE(config(t, "talkback_id=1&talkback_key=topsecret"));
  t.dev->http().queueTalkback("UNKNOWN topsecret");
  t.dev->app().upload();
  char diag[24000];
  gm::JsonWriter d(diag, sizeof(diag));
  t.dev->app().writeDiag(d);
  EXPECT_TRUE(d.ok());
  EXPECT_EQ(std::string(diag).find("topsecret"), std::string::npos);
  EXPECT_NE(std::string(diag).find("UNKNOWN"), std::string::npos);
}

namespace {
class TalkbackHttp : public gm::HttpClient {
 public:
  const char* reply = "";
  const char* expectedKey = "private-key";
  gm::HttpResult postJson(const char* url, const char* body, size_t len,
                          uint32_t timeout, char* resp, size_t cap) override {
    EXPECT_STREQ(
        url, "https://api.thingspeak.com/talkbacks/42/commands/execute.json");
    EXPECT_EQ(timeout, 3000u);
    EXPECT_EQ(len, strlen(body));
    EXPECT_EQ(std::string(url).find(expectedKey), std::string::npos);
    char key[33];
    EXPECT_TRUE(gm::jsonGetString(body, "api_key", key, sizeof(key)));
    EXPECT_STREQ(key, expectedKey);
    snprintf(resp, cap, "%s", reply);
    return {true, 200, "", 0};
  }
  gm::HttpResult get(const char*, uint32_t, char*, size_t) override {
    return {};
  }
  gm::HttpResult download(const char*, uint32_t, gm::DownloadSink&) override {
    return {};
  }
};
}  // namespace

TEST(Talkback, ExecuteRequestAndJsonResponse) {
  TalkbackHttp http;
  char command[128];
  EXPECT_EQ(gm::talkbackExecute(http, "https://api.thingspeak.com", 42,
                                http.expectedKey, command, sizeof(command)),
            gm::TalkbackResult::Empty);
  http.reply = " \r\n ";
  EXPECT_EQ(gm::talkbackExecute(http, "https://api.thingspeak.com", 42,
                                http.expectedKey, command, sizeof(command)),
            gm::TalkbackResult::Empty);
  http.reply = "{\"id\":12,\"command_string\":\"REMOTE 5 0\"}";
  EXPECT_EQ(gm::talkbackExecute(http, "https://api.thingspeak.com", 42,
                                http.expectedKey, command, sizeof(command)),
            gm::TalkbackResult::Command);
  EXPECT_STREQ(command, "REMOTE 5 0");
  // Real ThingSpeak responses (2026-11): a command, then an empty queue.
  http.reply =
      "{\"executed_at\":\"2026-11-26T16:03:09Z\",\"position\":null,"
      "\"id\":66879868,\"command_string\":\"REMOTE 10 0\","
      "\"created_at\":\"2026-11-26T16:03:09Z\"}";
  EXPECT_EQ(gm::talkbackExecute(http, "https://api.thingspeak.com", 42,
                                http.expectedKey, command, sizeof(command)),
            gm::TalkbackResult::Command);
  EXPECT_STREQ(command, "REMOTE 10 0");
  http.reply = "{}";
  EXPECT_EQ(gm::talkbackExecute(http, "https://api.thingspeak.com", 42,
                                http.expectedKey, command, sizeof(command)),
            gm::TalkbackResult::Empty);
  for (const char* reply : {"{\"command_string\":4}",
                            "{\"command_string\":\"REMOTE 5 0\"", "not json"}) {
    http.reply = reply;
    EXPECT_EQ(gm::talkbackExecute(http, "https://api.thingspeak.com", 42,
                                  http.expectedKey, command, sizeof(command)),
              gm::TalkbackResult::Failed);
  }
}

TEST(Talkback, PendingExpiresBeforeFirmwareTakesIt) {
  TestDevice t("remote_pending_expiry", 0);
  ASSERT_TRUE(config(t, "talkback_id=1&talkback_key=secret"));
  char command[80];
  snprintf(command, sizeof(command), "REMOTE 5 %lu",
           static_cast<unsigned long>(t.dev->clock().trueEpoch() + 60));
  t.dev->http().queueTalkback(command);
  t.dev->app().upload();
  t.dev->clock().advanceMs(61000);
  uint32_t minutes = 99;
  EXPECT_FALSE(t.dev->app().takeRemoteRequest(&minutes));
  EXPECT_EQ(minutes, 0u);
}
