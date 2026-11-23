// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "app/config.h"
#include "app/scheduler.h"
#include "ota/remote_update.h"
#include "util/json_read.h"
#include "util/sha256.h"

namespace {
std::string sha(const std::string& s) {
  gm::Sha256 h;
  h.update(s.data(), s.size());
  uint8_t d[32];
  h.finish(d);
  char hex[65];
  gm::sha256ToHex(d, hex);
  return hex;
}
}  // namespace

TEST(Sha256, Fips180Vectors) {
  EXPECT_EQ(sha(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(sha(std::string(1000000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, StreamingEqualsOneShot) {
  const std::string data(10007, 'x');
  gm::Sha256 h;
  for (size_t off = 0; off < data.size(); off += 61) {
    h.update(data.data() + off, std::min<size_t>(61, data.size() - off));
  }
  uint8_t d[32];
  h.finish(d);
  char hex[65];
  gm::sha256ToHex(d, hex);
  EXPECT_EQ(hex, sha(data));
  uint8_t back[32];
  ASSERT_TRUE(gm::sha256FromHex(hex, back));
  EXPECT_EQ(std::memcmp(back, d, 32), 0);
  EXPECT_FALSE(gm::sha256FromHex("abc", back));
}

TEST(JsonRead, TopLevelMembersOnly) {
  const char* j =
      "{ \"nested\": {\"version\": \"9\"}, \"list\": [1, {\"a\": 2}],"
      " \"version\" : \"1.2.3\", \"n\": 42, \"esc\": \"a\\\"b\\\\c\" }";
  char v[16];
  ASSERT_TRUE(gm::jsonGetString(j, "version", v, sizeof(v)));
  EXPECT_STREQ(v, "1.2.3");
  uint32_t n = 0;
  ASSERT_TRUE(gm::jsonGetUint(j, "n", &n));
  EXPECT_EQ(n, 42u);
  ASSERT_TRUE(gm::jsonGetString(j, "esc", v, sizeof(v)));
  EXPECT_STREQ(v, "a\"b\\c");
  EXPECT_FALSE(gm::jsonGetString(j, "missing", v, sizeof(v)));
  EXPECT_FALSE(gm::jsonGetUint(j, "version", &n));  // wrong type
  char tiny[3];
  EXPECT_FALSE(gm::jsonGetString(j, "version", tiny, sizeof(tiny)));
  EXPECT_FALSE(gm::jsonGetString("{\"version\": \"1.2", "version", v, 16));
  EXPECT_FALSE(gm::jsonGetString("[]", "version", v, 16));
}

TEST(Manifest, ParsesFirmwareAndServiceRequest) {
  const std::string hash(64, 'a');
  const std::string j =
      "{\"version\":\"0.2.0\",\"url\":\"https://h/f.bin\","
      "\"sha256\":\"" +
      hash + "\",\"service_id\":3,\"service_min\":10}";
  gm::UpdateManifest m;
  const char* err = nullptr;
  ASSERT_TRUE(gm::parseUpdateManifest(j.c_str(), &m, &err)) << err;
  EXPECT_TRUE(m.hasFirmware);
  EXPECT_STREQ(m.version, "0.2.0");
  EXPECT_EQ(m.sha256[0], 0xAA);
  EXPECT_EQ(m.serviceId, 3u);
  EXPECT_EQ(m.serviceMin, 10u);
}

TEST(Manifest, RejectsIncompleteOrInvalid) {
  gm::UpdateManifest m;
  const char* err = nullptr;
  EXPECT_FALSE(gm::parseUpdateManifest(
      "{\"version\":\"0.2.0\",\"url\":\"https://h/f.bin\"}", &m, &err));
  EXPECT_FALSE(gm::parseUpdateManifest(
      "{\"version\":\"0.2.0\",\"url\":\"u\",\"sha256\":\"xyz\"}", &m, &err));
  EXPECT_FALSE(gm::parseUpdateManifest("{\"service_id\":1}", &m, &err));
  EXPECT_FALSE(gm::parseUpdateManifest("{}", &m, &err));
  EXPECT_TRUE(gm::parseUpdateManifest("{\"service_id\":1,\"service_min\":5}",
                                      &m, &err));
  EXPECT_FALSE(m.hasFirmware);
}

TEST(Manifest, VersionOrdering) {
  EXPECT_LT(gm::compareVersions("0.1.0", "0.2.0"), 0);
  EXPECT_GT(gm::compareVersions("1.10.0", "1.9.9"), 0);
  EXPECT_EQ(gm::compareVersions("1.2", "1.2.0"), 0);
  EXPECT_GT(gm::compareVersions("1.2.1", "1.2"), 0);
  EXPECT_LT(gm::compareVersions("1.0.0-rc1", "1.0.0"), 0);
  EXPECT_GT(gm::compareVersions("1.0.0", "1.0.0-dev"), 0);
  EXPECT_EQ(gm::compareVersions("2.0.0", "2.0.0"), 0);
}

class PowerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    st.reset(new gm::RetainedState());
    st->init();
    gm::configDefaults(&cfg, "https://api.thingspeak.com");
    cfg.channelId = 1;
    std::snprintf(cfg.writeKey, sizeof(cfg.writeKey), "ABCDEFGH12345678");
  }
  std::unique_ptr<gm::RetainedState> st;
  gm::Config cfg;
};

TEST_F(PowerTest, ModesFromThresholds) {
  EXPECT_EQ(gm::powerModeFor(-1, cfg), gm::PowerMode::Normal);  // unknown
  EXPECT_EQ(gm::powerModeFor(3900, cfg), gm::PowerMode::Normal);
  EXPECT_EQ(gm::powerModeFor(3400, cfg), gm::PowerMode::Economy);
  EXPECT_EQ(gm::powerModeFor(3200, cfg), gm::PowerMode::Critical);
  cfg.battLowMv = 0;
  cfg.battCritMv = 0;
  EXPECT_EQ(gm::powerModeFor(3000, cfg), gm::PowerMode::Normal);  // disabled
}

TEST_F(PowerTest, EconomyAndCriticalSchedules) {
  for (int i = 0; i < 4; ++i) {
    gm::Sample s = {};
    st->push(s);
  }
  st->hasMeasured = 1;
  st->lastMeasureMonoS = 7200;
  st->hasUploadAttempt = 1;
  st->lastUploadAttemptMonoS = 3600;
  EXPECT_TRUE(gm::schedulePlan(*st, cfg, 7200).uploadDue);
  EXPECT_FALSE(
      gm::schedulePlan(*st, cfg, 7200, gm::PowerMode::Economy).uploadDue);
  EXPECT_TRUE(
      gm::schedulePlan(*st, cfg, 3600 + 4 * 3600, gm::PowerMode::Economy)
          .uploadDue);
  const gm::Plan crit =
      gm::schedulePlan(*st, cfg, 7200 + 900, gm::PowerMode::Critical);
  EXPECT_FALSE(crit.uploadDue);
  EXPECT_FALSE(crit.measureDue);  // interval doubled to 30 min
  EXPECT_EQ(crit.sleepS, 900u);
}

TEST(ConfigNew, ValidatesNetworkPowerAndUpdateFields) {
  gm::Config cfg;
  gm::configDefaults(&cfg, "https://api.thingspeak.com");
  const gm::ConfigLimits esp = {60, false};
  char err[96];
  auto apply = [&](const char* body) {
    return gm::configApplyForm(gm::Form(body, std::strlen(body)), esp, &cfg,
                               err, sizeof(err));
  };
  EXPECT_TRUE(
      apply("static_ip=192.168.1.50&gateway=192.168.1.1&netmask=255."
            "255.255.0&dns=1.1.1.1"));
  EXPECT_STREQ(cfg.staticIp, "192.168.1.50");
  EXPECT_FALSE(apply("static_ip=192.168.1.300"));
  EXPECT_FALSE(apply("static_ip=10.0.0.5&gateway=&netmask=255.0.0.0"));
  EXPECT_TRUE(apply("static_ip=&gateway=&netmask=&dns="));  // back to DHCP
  EXPECT_FALSE(cfg.staticIpEnabled());
  EXPECT_FALSE(apply("batt_low_mv=3300&batt_crit_mv=3400"));
  EXPECT_FALSE(apply("batt_low_mv=1200"));
  EXPECT_TRUE(apply("batt_low_mv=0&batt_crit_mv=3300"));
  EXPECT_FALSE(apply("update_url=http%3A%2F%2Fexample.com%2Fm.json"));
  EXPECT_TRUE(
      apply("update_url=https%3A%2F%2Fexample.com%2Fm.json&update_check_h=12"));
  EXPECT_TRUE(cfg.remoteUpdateEnabled());
  EXPECT_TRUE(apply("diag_every_h=0"));
  uint8_t ip[4];
  EXPECT_TRUE(gm::parseIpv4("10.1.2.3", ip));
  EXPECT_EQ(ip[3], 3);
  EXPECT_FALSE(gm::parseIpv4("10.1.2", ip));
  EXPECT_FALSE(gm::parseIpv4("10.1.2.3.4", ip));
  EXPECT_FALSE(gm::parseIpv4("1..2.3", ip));
}
