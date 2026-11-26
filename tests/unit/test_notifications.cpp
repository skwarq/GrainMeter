// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "app/notifications.h"
namespace {
class CaptureHttp : public gm::HttpClient {
 public:
  std::string url, body;
  const char* response = "{\"ok\" : true}";
  gm::HttpResult postJson(const char* u, const char* b, size_t n, uint32_t,
                          char* resp, size_t cap) override {
    url = u;
    body.assign(b, n);
    std::snprintf(resp, cap, "%s", response);
    gm::HttpResult r = {};
    r.transportOk = true;
    r.status = 200;
    return r;
  }
  gm::HttpResult get(const char*, uint32_t, char*, size_t) override {
    return {};
  }
  gm::HttpResult download(const char*, uint32_t, gm::DownloadSink&) override {
    return {};
  }
};
}  // namespace
TEST(Notifications, NtfyJsonUsesServerOriginAndTopic) {
  gm::Config c;
  gm::configDefaults(&c, "mock://thingspeak");
  std::strcpy(c.notifyNtfyUrl, "https://ntfy.sh/silos");
  CaptureHttp http;
  const auto r = gm::notifySend(c, http, 0, "Alarm \"punkt 1\"");
  EXPECT_TRUE(r.enabled);
  EXPECT_TRUE(r.ok);
  EXPECT_EQ(http.url, "https://ntfy.sh");
  EXPECT_NE(http.body.find("\"topic\":\"silos\""), std::string::npos);
  EXPECT_NE(http.body.find("\"priority\":4"), std::string::npos);
  EXPECT_NE(http.body.find("\\\"punkt 1\\\""), std::string::npos);
}
TEST(Notifications, TelegramPayloadAndApiFailure) {
  gm::Config c;
  gm::configDefaults(&c, "mock://thingspeak");
  std::strcpy(c.notifyTgToken, "123:secret");
  std::strcpy(c.notifyTgChat, "-123");
  CaptureHttp http;
  EXPECT_TRUE(gm::notifySend(c, http, 1, "Test").ok);
  EXPECT_EQ(http.url, "https://api.telegram.org/bot123:secret/sendMessage");
  EXPECT_EQ(http.body, "{\"chat_id\":\"-123\",\"text\":\"Test\"}");
  http.response = "{\"ok\":false}";
  EXPECT_FALSE(gm::notifySend(c, http, 1, "Test").ok);
  c.notifyTgChat[0] = 0;
  EXPECT_FALSE(gm::notifySend(c, http, 1, "Test").enabled);
}
