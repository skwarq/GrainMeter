// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <map>
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

TEST_F(ConfigTest, LogDestinationValidationAndJson) {
  EXPECT_EQ(cfg.logDest, 0u);
  ASSERT_TRUE(apply("log_udp=1&log_dest=mqtt&log_level=2", &cfg, &err));
  EXPECT_EQ(cfg.logDest, 1u);
  EXPECT_EQ(cfg.logUdp, 1u);
  EXPECT_EQ(cfg.logLevel, 2u);
  char json[4096];
  gm::JsonWriter w(json, sizeof(json));
  gm::configWriteJson(w, cfg);
  ASSERT_TRUE(w.ok());
  EXPECT_NE(std::string(json).find("\"log_dest\":\"mqtt\""), std::string::npos);
  for (const char* form : {"log_dest=", "log_dest=1", "log_dest=MQTT",
                           "log_dest=tcp", "log_dest=%00"}) {
    const gm::Config before = cfg;
    EXPECT_FALSE(apply(form, &cfg, &err)) << form;
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(std::memcmp(&before, &cfg, sizeof(cfg)), 0);
  }
  ASSERT_TRUE(apply("log_dest=udp", &cfg, &err));
  EXPECT_EQ(cfg.logDest, 0u);
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

TEST_F(ConfigTest, RemoteValidationAndSecrets) {
  EXPECT_EQ(cfg.remoteBudgetMin, 60u);
  EXPECT_EQ(cfg.remoteIdleMin, 10u);
  ASSERT_TRUE(apply("remote_idle_min=1", &cfg, &err));
  ASSERT_TRUE(apply("remote_idle_min=60", &cfg, &err));
  ASSERT_TRUE(
      apply("remote_url=mqtts://"
            "broker.example:8883&remote_user=device&remote_pass=broker-secret&"
            "remote_id=gm-0123&talkback_id=4294967295&talkback_key=talk-secret&"
            "remote_budget_min=1440",
            &cfg, &err));
  EXPECT_EQ(cfg.talkbackId, UINT32_MAX);
  char json[4096];
  gm::JsonWriter w(json, sizeof(json));
  gm::configWriteJson(w, cfg);
  EXPECT_TRUE(w.ok());
  EXPECT_EQ(std::string(json).find("broker-secret"), std::string::npos);
  EXPECT_EQ(std::string(json).find("talk-secret"), std::string::npos);
  EXPECT_NE(std::string(json).find("\"remote_pass_set\":true"),
            std::string::npos);
  EXPECT_NE(std::string(json).find("\"talkback_key_set\":true"),
            std::string::npos);
  EXPECT_NE(std::string(json).find("\"remote_idle_min\":60"),
            std::string::npos);
  ASSERT_TRUE(apply("remote_pass=&talkback_key=", &cfg, &err));
  EXPECT_STREQ(cfg.remotePass, "broker-secret");
  for (const char* s :
       {"remote_url=http://broker", "remote_url=mqtts://",
        "remote_url=mqtts://host:0", "remote_url=mqtts://host:65536",
        "remote_url=mqtts://user@host", "remote_url=mqtts://host/path",
        "remote_id=Bad_ID", "remote_budget_min=1441", "talkback_id=4294967296",
        "talkback_id=-1", "talkback_id=%2B1", "remote_idle_min=0",
        "remote_idle_min=61", "remote_idle_min=-1", "remote_idle_min=1.5",
        "remote_idle_min="}) {
    const gm::Config before = cfg;
    EXPECT_FALSE(apply(s, &cfg, &err)) << s;
    EXPECT_EQ(std::memcmp(&before, &cfg, sizeof(cfg)), 0);
  }
  EXPECT_TRUE(
      apply("remote_url=&remote_id=&remote_budget_min=0&talkback_id=0&remote_"
            "pass_clear=1&talkback_key_clear=1",
            &cfg, &err));
  EXPECT_STREQ(cfg.remotePass, "");
  EXPECT_STREQ(cfg.talkbackKey, "");
}

namespace {
class RemoteStore : public gm::KeyValueStore {
 public:
  std::map<std::string, std::string> values;
  bool get(const char* key, char* out, size_t cap) override {
    const auto it = values.find(key);
    if (it == values.end()) return false;
    snprintf(out, cap, "%s", it->second.c_str());
    return true;
  }
  bool set(const char* key, const char* value) override {
    EXPECT_LE(strlen(key), 15u);
    values[key] = value;
    return true;
  }
  bool commit() override { return true; }
};
}  // namespace

TEST_F(ConfigTest, LogDestinationPersistenceAndLegacyDefault) {
  RemoteStore store;
  store.values["log_udp"] = "1";
  gm::RomMap map;
  const char* error = nullptr;
  ASSERT_TRUE(gm::configLoad(store, &cfg, &map, &error));
  EXPECT_EQ(cfg.logUdp, 1u);
  EXPECT_EQ(cfg.logDest, 0u);
  ASSERT_TRUE(apply("log_dest=mqtt", &cfg, &err));
  ASSERT_TRUE(gm::configSave(store, cfg));
  EXPECT_EQ(store.values["log_dest"], "1");
  gm::configDefaults(&cfg, "https://api.thingspeak.com");
  ASSERT_TRUE(gm::configLoad(store, &cfg, &map, &error));
  EXPECT_EQ(cfg.logDest, 1u);
  for (const char* value : {"2", "-1", "mqtt", ""}) {
    store.values["log_dest"] = value;
    gm::configDefaults(&cfg, "https://api.thingspeak.com");
    EXPECT_FALSE(gm::configLoad(store, &cfg, &map, &error));
    EXPECT_NE(error, nullptr);
    EXPECT_EQ(cfg.logDest, 0u);
  }
  ASSERT_TRUE(apply("log_dest=udp", &cfg, &err));
  ASSERT_TRUE(gm::configSave(store, cfg));
  EXPECT_EQ(store.values["log_dest"], "0");
}

TEST_F(ConfigTest, RemotePersistenceAndStoredValidation) {
  ASSERT_TRUE(apply(
      "remote_url=mqtts://"
      "broker:8883&remote_user=user&remote_pass=password&remote_id=gm-test&"
      "talkback_id=4294967295&talkback_key=private&remote_budget_min=1440&"
      "remote_idle_min=23",
      &cfg, &err));
  RemoteStore store;
  ASSERT_TRUE(gm::configSave(store, cfg));
  gm::Config loaded;
  gm::configDefaults(&loaded, "https://api.thingspeak.com");
  gm::RomMap map;
  const char* error = nullptr;
  ASSERT_TRUE(gm::configLoad(store, &loaded, &map, &error));
  EXPECT_STREQ(loaded.remoteUrl, cfg.remoteUrl);
  EXPECT_STREQ(loaded.remoteUser, cfg.remoteUser);
  EXPECT_STREQ(loaded.remotePass, cfg.remotePass);
  EXPECT_STREQ(loaded.remoteId, cfg.remoteId);
  EXPECT_STREQ(loaded.talkbackKey, cfg.talkbackKey);
  EXPECT_EQ(loaded.talkbackId, cfg.talkbackId);
  EXPECT_EQ(loaded.remoteBudgetMin, cfg.remoteBudgetMin);
  EXPECT_EQ(loaded.remoteIdleMin, 23u);
  EXPECT_EQ(store.values["remote_idle_min"], "23");
  store.values["remote_url"] = "https://wrong";
  store.values["remote_id"] = "INVALID";
  store.values["remote_budget_m"] = "1441";
  store.values["talkback_id"] = "4294967296";
  store.values["remote_idle_min"] = "61";
  gm::configDefaults(&loaded, "https://api.thingspeak.com");
  EXPECT_FALSE(gm::configLoad(store, &loaded, &map, &error));
  EXPECT_STREQ(loaded.remoteUrl, "");
  EXPECT_STREQ(loaded.remoteId, "");
  EXPECT_EQ(loaded.talkbackId, 0u);
  EXPECT_EQ(loaded.remoteBudgetMin, 60u);
  EXPECT_EQ(loaded.remoteIdleMin, 10u);
}

TEST_F(ConfigTest, BatteryCapacityValidationAndPersistence) {
  EXPECT_EQ(cfg.battCapacityMah, 2500u);
  for (const char* form : {"batt_capacity_mah=99", "batt_capacity_mah=20001",
                           "batt_capacity_mah=-1", "batt_capacity_mah=2500.5",
                           "batt_capacity_mah="}) {
    const gm::Config before = cfg;
    EXPECT_FALSE(apply(form, &cfg, &err));
    EXPECT_EQ(std::memcmp(&before, &cfg, sizeof(cfg)), 0);
    EXPECT_FALSE(err.empty());
  }
  ASSERT_TRUE(apply("batt_capacity_mah=100", &cfg, &err));
  ASSERT_TRUE(apply("batt_capacity_mah=20000", &cfg, &err));
  RemoteStore store;
  ASSERT_TRUE(gm::configSave(store, cfg));
  gm::Config loaded;
  gm::configDefaults(&loaded, "https://api.thingspeak.com");
  gm::RomMap map;
  const char* error = nullptr;
  ASSERT_TRUE(gm::configLoad(store, &loaded, &map, &error));
  EXPECT_EQ(loaded.battCapacityMah, 20000u);
  char out[4096];
  gm::JsonWriter w(out, sizeof(out));
  gm::configWriteJson(w, loaded);
  ASSERT_TRUE(w.ok());
  EXPECT_NE(std::string(out).find("\"batt_capacity_mah\":20000"),
            std::string::npos);
  store.values["batt_capacity"] = "99";
  gm::configDefaults(&loaded, "https://api.thingspeak.com");
  EXPECT_FALSE(gm::configLoad(store, &loaded, &map, &error));
  EXPECT_NE(error, nullptr);
  EXPECT_EQ(loaded.battCapacityMah, 2500u);
}
