// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "app/config.h"

namespace {
const gm::ConfigLimits kEsp = {60, false};
const gm::ConfigLimits kPc = {5, true};

bool apply(const char* body, gm::Config* cfg, std::string* err,
           gm::ConfigLimits lim = kEsp) {
  char e[96] = "";
  const bool ok = gm::configApplyForm(gm::Form(body, std::strlen(body)), lim,
                                      cfg, e, sizeof(e));
  *err = e;
  return ok;
}
}  // namespace

class ConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    gm::configDefaults(&cfg, "https://api.thingspeak.com");
  }
  gm::Config cfg;
  std::string err;
};

TEST_F(ConfigTest, AppliesValidForm) {
  ASSERT_TRUE(
      apply("wifi_ssid=Farm+WiFi&wifi_pass=secret%21pass&ts_channel=123456&ts_"
            "key=ABCDEFGH12345678"
            "&measure_s=600&upload_s=3600",
            &cfg, &err))
      << err;
  EXPECT_STREQ(cfg.wifiSsid, "Farm WiFi");
  EXPECT_STREQ(cfg.wifiPass, "secret!pass");
  EXPECT_EQ(cfg.channelId, 123456u);
  EXPECT_EQ(cfg.samplesPerUpload(), 6u);
}

TEST_F(ConfigTest, EmptySecretKeepsValue) {
  ASSERT_TRUE(
      apply("wifi_pass=first-pass&ts_key=ABCDEFGH12345678", &cfg, &err));
  ASSERT_TRUE(apply("wifi_pass=&ts_key=&wifi_ssid=x", &cfg, &err));
  EXPECT_STREQ(cfg.wifiPass, "first-pass");
  EXPECT_STREQ(cfg.writeKey, "ABCDEFGH12345678");
  ASSERT_TRUE(apply("wifi_pass_clear=1", &cfg, &err));
  EXPECT_STREQ(cfg.wifiPass, "");
}

TEST_F(ConfigTest, InvalidFormLeavesConfigUntouched) {
  const gm::Config before = cfg;
  EXPECT_FALSE(
      apply("wifi_ssid=ok&measure_s=10", &cfg, &err));  // < 60 s on ESP32
  EXPECT_FALSE(err.empty());
  EXPECT_EQ(std::memcmp(&before, &cfg, sizeof(cfg)), 0);
  EXPECT_FALSE(apply("measure_s=3600&upload_s=900", &cfg, &err));
  EXPECT_FALSE(apply("ts_key=short", &cfg, &err));
  EXPECT_FALSE(apply("ts_key=ABCDEFGH-2345678", &cfg, &err));
  EXPECT_FALSE(apply("wifi_pass=1234567", &cfg, &err));
  EXPECT_FALSE(apply("svc_pass=has+space", &cfg, &err));
  EXPECT_FALSE(apply("ts_channel=-5", &cfg, &err));
  EXPECT_FALSE(apply("ts_channel=12a", &cfg, &err));
  EXPECT_FALSE(apply("wifi_ssid=%ZZ", &cfg, &err));
  EXPECT_FALSE(apply("wifi_ssid=%00", &cfg, &err));
  EXPECT_FALSE(
      apply("wifi_ssid=123456789012345678901234567890123", &cfg, &err));
}

TEST_F(ConfigTest, ReportUrlSchemesPerPlatform) {
  EXPECT_FALSE(apply("report_url=http%3A%2F%2Fexample.com", &cfg, &err));
  EXPECT_FALSE(apply("report_url=mock%3A%2F%2Fthingspeak", &cfg, &err));
  EXPECT_TRUE(
      apply("report_url=https%3A%2F%2Fapi.thingspeak.com%2F", &cfg, &err));
  EXPECT_STREQ(cfg.reportUrl,
               "https://api.thingspeak.com");  // trailing / removed
  EXPECT_TRUE(apply("report_url=mock%3A%2F%2Fthingspeak", &cfg, &err, kPc));
}

TEST_F(ConfigTest, JsonNeverContainsSecrets) {
  ASSERT_TRUE(
      apply("wifi_pass=TopSecretWifi&ts_key=SECRETKEY0000001&diag_key="
            "SECRETKEY0000002"
            "&svc_pass=ServicePass1",
            &cfg, &err));
  char out[1024];
  gm::JsonWriter w(out, sizeof(out));
  gm::configWriteJson(w, cfg);
  ASSERT_TRUE(w.ok());
  const std::string j = out;
  EXPECT_EQ(j.find("TopSecretWifi"), std::string::npos);
  EXPECT_EQ(j.find("SECRETKEY"), std::string::npos);
  EXPECT_EQ(j.find("ServicePass1"), std::string::npos);
  EXPECT_NE(j.find("\"wifi_pass_set\":true"), std::string::npos);
  EXPECT_NE(j.find("\"svc_pass_set\":true"), std::string::npos);
}

TEST_F(ConfigTest, UdpLogSettings) {
  EXPECT_EQ(cfg.logUdp, 0u);
  EXPECT_EQ(cfg.logLevel, 3u);
  EXPECT_EQ(cfg.logPort, 5514u);
  EXPECT_STREQ(cfg.logHost, "");
  ASSERT_TRUE(apply(
      "log_udp=1&log_level=4&log_host=192.168.0.105&log_port=9000", &cfg, &err))
      << err;
  EXPECT_EQ(cfg.logUdp, 1u);
  EXPECT_EQ(cfg.logLevel, 4u);
  EXPECT_STREQ(cfg.logHost, "192.168.0.105");
  EXPECT_EQ(cfg.logPort, 9000u);
  ASSERT_TRUE(apply("log_host=", &cfg, &err));  // broadcast
  EXPECT_STREQ(cfg.logHost, "");
  EXPECT_FALSE(apply("log_udp=2", &cfg, &err));
  EXPECT_FALSE(apply("log_level=0", &cfg, &err));
  EXPECT_FALSE(apply("log_port=70000", &cfg, &err));
  EXPECT_FALSE(apply("log_host=pc.local", &cfg, &err));
}

TEST_F(ConfigTest, LanWakeOnByDefault) {
  EXPECT_EQ(cfg.lanWake, 1u);
  ASSERT_TRUE(apply("lan_wake=0", &cfg, &err)) << err;
  EXPECT_EQ(cfg.lanWake, 0u);
  EXPECT_FALSE(apply("lan_wake=2", &cfg, &err));
}

TEST_F(ConfigTest, SoftApDelayAndSwitch) {
  EXPECT_EQ(cfg.apEnabled, 1u);
  EXPECT_EQ(cfg.apDelayS, 60u);
  ASSERT_TRUE(apply("ap_enabled=0&ap_delay_s=0", &cfg, &err)) << err;
  EXPECT_EQ(cfg.apEnabled, 0u);
  EXPECT_EQ(cfg.apDelayS, 0u);
  EXPECT_FALSE(apply("ap_delay_s=3601", &cfg, &err));
  EXPECT_FALSE(apply("ap_enabled=yes", &cfg, &err));
}
