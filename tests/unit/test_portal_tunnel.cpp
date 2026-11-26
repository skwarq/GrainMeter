// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app/portal_tunnel.h"
#include "support/gm2.h"
#include "support/test_device.h"
#include "util/base64url.h"
#include "util/hmac_sha256.h"
#include "util/json_read.h"
namespace {
const char* nonce = "0123456789abcdef0123456789abcdef";
class TunnelTest : public ::testing::Test {
 protected:
  TestDevice device{"tunnel"};
  gm::Portal portal{device.dev->app()};
  gm::PortalTunnel tunnel{portal, device.dev->app()};
  gm::TunnelOut out{};
  uint64_t responseSeq = 1;
  std::string responseMid = "m1", responsePassword = "test-password";
  std::string bodyText() {
    return gm2test::plaintext(out.payload, "gm-test", nonce, responseSeq,
                              responseMid.c_str(), responsePassword.c_str());
  }
  bool readBody(char* buf, size_t cap) {
    std::string text = bodyText();
    if (text.size() >= cap) return false;
    memcpy(buf, text.data(), text.size());
    buf[text.size()] = 0;
    return true;
  }
  void SetUp() override {
    ASSERT_TRUE(device.started);
    tunnel.beginSession(nonce, "gm-test");
  }
  std::string request(uint64_t seq = 1, const char* method = "GET",
                      const char* path = "/api/status", const char* body = "",
                      const char* mid = "m1", const char* key = "test-password",
                      const char* cid = gm2test::cid) {
    responseSeq = seq;
    responseMid = mid;
    responsePassword = key;
    return gm2test::request("gm-test", nonce, seq, method, path, body, mid, key,
                            cid);
  }
  void run(const std::string& payload, const char* mid = "m1",
           uint32_t now = 0) {
    ASSERT_TRUE(
        tunnel.handle(mid, payload.data(), payload.size(), false, now, &out));
    ASSERT_EQ(out.len, strlen(out.payload));
  }
  void checkMac(uint64_t seq = 1, const char* mid = "m1",
                const char* key = "test-password") {
    responseSeq = seq;
    responseMid = mid;
    responsePassword = key;
    EXPECT_TRUE(
        gm2test::validMac(out.payload, "gm-test", nonce, seq, mid, key));
  }
};
TEST_F(TunnelTest, StatusConfigAndPasswordChange) {
  run(request());
  ASSERT_EQ(out.status, 200);
  checkMac();
  run(request(2, "POST", "/api/config", "name=Tunnel%20test"));
  ASSERT_EQ(out.status, 200);
  checkMac(2);
  EXPECT_STREQ(device.dev->app().config().deviceName, "Tunnel test");
  run(request(3, "POST", "/api/config", "svc_pass=changed-password"));
  ASSERT_EQ(out.status, 200);
  checkMac(3);
  EXPECT_STREQ(device.dev->app().config().servicePassword, "changed-password");
  run(request(4, "GET", "/api/status", "", "m1", "changed-password"));
  ASSERT_EQ(out.status, 200);
  checkMac(4, "m1", "changed-password");
}

TEST_F(TunnelTest, LogPreviewUsesSignedTunnelAndBoundedCursorPages) {
  run(request(1, "GET", "/api/logs/0"));
  ASSERT_EQ(out.status, 200);
  EXPECT_LT(out.len, 20u * 1024);
  checkMac();
  char body[8192];
  ASSERT_TRUE(readBody(body, sizeof(body)));
  uint32_t next = 0;
  ASSERT_TRUE(gm::jsonGetUint(body, "next", &next));
  EXPECT_NE(std::string(body).find("pc: simulated log preview"),
            std::string::npos);
  const std::string path = "/api/logs/" + std::to_string(next);
  run(request(2, "GET", path.c_str()));
  ASSERT_EQ(out.status, 200);
  checkMac(2);
  ASSERT_TRUE(readBody(body, sizeof(body)));
  EXPECT_NE(std::string(body).find("\"lost\":0"), std::string::npos);
  run(request(3, "GET", "/api/logs/0", "", "m1", "wrong"));
  EXPECT_EQ(out.status, 401);
}

TEST_F(TunnelTest, WorstCaseEscapedLogPagesFitResponseEnvelope) {
  class BufferedDevice : public gmpc::PcDevice {
   public:
    explicit BufferedDevice(const Options& options) : PcDevice(options) {}
    gm::LogSource* logSource() override { return &logs; }
    gm::LogBuffer logs;
  };
  gmpc::PcDevice::Options options;
  options.dataDir = device.dir + "/bounded-logs";
  options.servicePassword = "test-password";
  BufferedDevice bounded(options);
  std::string error;
  ASSERT_TRUE(bounded.start(&error)) << error;
  const std::string line = std::string(255, '\1') + "\n";
  for (int i = 0; i < 16; ++i) bounded.logs.append(line.data(), line.size());
  gm::Portal logPortal(bounded.app());
  gm::PortalTunnel logTunnel(logPortal, bounded.app());
  logTunnel.beginSession(nonce, "gm-test");
  uint32_t next = 0;
  for (uint64_t seq = 1; next < 16; ++seq) {
    const std::string path = "/api/logs/" + std::to_string(next);
    const std::string payload = request(seq, "GET", path.c_str());
    ASSERT_TRUE(
        logTunnel.handle("m1", payload.data(), payload.size(), false, 0, &out));
    ASSERT_EQ(out.status, 200);
    EXPECT_LT(out.len, 20u * 1024);
    checkMac(seq);
    char body[8192];
    ASSERT_TRUE(readBody(body, sizeof(body)));
    EXPECT_LT(strlen(body), 8192u);
    uint32_t delivered = 0, lost = 1;
    ASSERT_TRUE(gm::jsonGetUint(body, "next", &delivered));
    ASSERT_TRUE(gm::jsonGetUint(body, "lost", &lost));
    ASSERT_GT(delivered, next);
    EXPECT_EQ(lost, 0u);
    next = delivered;
  }
}

TEST_F(TunnelTest, RemoteExtendLimitsBudgetClockAndExit) {
  run(request(1, "POST", "/api/remote/extend", "minutes=10"));
  EXPECT_EQ(out.status, 409);
  EXPECT_NE(bodyText().find("no_session"), std::string::npos);
  run(request(
      2, "POST", "/api/config",
      "remote_url=mqtts://broker&remote_budget_min=120&remote_idle_min=60"));
  ASSERT_EQ(out.status, 200);
  run(request(3, "POST", "/api/sim", "action=remote_start&minutes=10"));
  ASSERT_EQ(out.status, 200);
  run(request(4, "POST", "/api/remote/extend", "minutes=60"));
  ASSERT_EQ(out.status, 200);
  checkMac(4);
  char body[512];
  ASSERT_TRUE(readBody(body, sizeof(body)));
  EXPECT_NE(std::string(body).find("\"until\":0"), std::string::npos);
  EXPECT_NE(std::string(body).find("\"remaining_s\":3600"), std::string::npos);
  EXPECT_NE(std::string(body).find("\"budget_left_s\":7200"),
            std::string::npos);
  const std::string cached = out.payload;
  run(request(4, "POST", "/api/remote/extend", "minutes=60"));
  EXPECT_EQ(out.payload, cached);
  device.dev->clock().advanceMs(30000);
  ASSERT_TRUE(device.dev->clock().setEpoch(1800000000));
  run(request(5, "POST", "/api/remote/extend", "minutes=1"));
  ASSERT_EQ(out.status, 200);
  ASSERT_TRUE(readBody(body, sizeof(body)));
  EXPECT_NE(std::string(body).find("\"until\":1800003600"), std::string::npos);
  EXPECT_NE(std::string(body).find("\"remaining_s\":3600"), std::string::npos);
  EXPECT_EQ(device.dev->app().state().remoteUsedS, 30u);
  ASSERT_TRUE(device.dev->clock().setEpoch(1799990000));
  run(request(6, "POST", "/api/remote/extend", "minutes=1"));
  ASSERT_EQ(out.status, 200);
  ASSERT_TRUE(readBody(body, sizeof(body)));
  EXPECT_NE(std::string(body).find("\"until\":1799993600"), std::string::npos);
  run(request(7, "POST", "/api/config", "remote_budget_min=2"));
  run(request(8, "POST", "/api/remote/extend", "minutes=10"));
  ASSERT_EQ(out.status, 200);
  ASSERT_TRUE(readBody(body, sizeof(body)));
  EXPECT_NE(std::string(body).find("\"remaining_s\":90"), std::string::npos);
  EXPECT_NE(std::string(body).find("\"budget_left_s\":90"), std::string::npos);
  device.dev->clock().advanceMs(10000);
  run(request(9, "POST", "/api/service/exit"));
  ASSERT_EQ(out.status, 200);
  checkMac(9);
  EXPECT_FALSE(device.dev->active());
  EXPECT_EQ(device.dev->app().state().remoteUsedS, 40u);
  run(request(10, "POST", "/api/remote/extend", "minutes=10"));
  EXPECT_EQ(out.status, 409);
}

TEST_F(TunnelTest, RemoteExtendExhaustedAndInvalidMinutes) {
  run(request(1, "POST", "/api/config",
              "remote_url=mqtts://broker&remote_budget_min=1"));
  run(request(2, "POST", "/api/sim", "action=remote_start&minutes=1"));
  ASSERT_EQ(out.status, 200);
  uint64_t seq = 3;
  for (const char* body : {"", "minutes=0", "minutes=61", "minutes=1.5"}) {
    run(request(seq++, "POST", "/api/remote/extend", body));
    EXPECT_EQ(out.status, 400);
  }
  device.dev->clock().advanceMs(60000);
  run(request(20, "POST", "/api/remote/extend", "minutes=1"));
  EXPECT_EQ(out.status, 409);
  EXPECT_NE(bodyText().find("budget"), std::string::npos);
  EXPECT_EQ(device.dev->app().state().remoteUsedS, 60u);
}

TEST_F(TunnelTest, RemoteIdleCountsAuthenticatedRequestsOnly) {
  run(request(1, "POST", "/api/config",
              "remote_url=mqtts://broker&remote_idle_min=1"));
  run(request(2, "POST", "/api/sim", "action=remote_start&minutes=10"));
  ASSERT_EQ(out.status, 200);
  device.dev->clock().advanceMs(50000);
  run(request(3, "GET", "/api/config"));
  ASSERT_EQ(out.status, 200);
  device.dev->clock().advanceMs(50000);
  run(request(4, "GET", "/api/status", "", "m1", "wrong"));
  EXPECT_EQ(out.status, 401);
  device.dev->clock().advanceMs(10000);
  device.dev->tick();
  EXPECT_FALSE(device.dev->active());
  EXPECT_EQ(device.dev->app().state().remoteUsedS, 110u);
}
TEST_F(TunnelTest, BackgroundPollsDoNotKeepSessionAlive) {
  run(request(1, "POST", "/api/config",
              "remote_url=mqtts://broker&remote_idle_min=1"));
  run(request(2, "POST", "/api/sim", "action=remote_start&minutes=10"));
  ASSERT_EQ(out.status, 200);
  // A forgotten tab: status, logs and history every 20 s for 80 s.
  uint64_t seq = 3;
  for (int i = 0; i < 4; ++i) {
    device.dev->clock().advanceMs(20000);
    run(request(seq++, "GET", "/api/status"));
    ASSERT_EQ(out.status, 200);
    run(request(seq++, "GET", "/api/logs/0"));
    run(request(seq++, "GET", "/api/history"));
  }
  device.dev->tick();
  EXPECT_FALSE(device.dev->active());
}
TEST_F(TunnelTest, LockoutSeparateFromLanAndExpires) {
  std::string bad = request(1, "GET", "/api/status", "", "m1", "wrong");
  for (int i = 0; i < 5; ++i) {
    run(bad, "m1", 10);
    EXPECT_EQ(out.status, 401);
    EXPECT_NE(std::string(out.payload).find("\"mac\":\"\""), std::string::npos);
  }
  EXPECT_TRUE(portal.authorized("test-password"));
  run(request(), "m1", 10);
  EXPECT_EQ(out.status, 429);
  EXPECT_NE(std::string(out.payload).find("retry_s\\\":60"), std::string::npos);
  run(request(1, "GET", "/api/status", "", "m1", "test-password",
              "ffffffffffffffff"),
      "m1", 10);
  EXPECT_EQ(out.status, 429);
  run(request(), "m1", 69);
  EXPECT_EQ(out.status, 429);
  run(request(), "m1", 70);
  EXPECT_EQ(out.status, 200);
  checkMac();
}
TEST_F(TunnelTest, EmptyServicePasswordRefusesTunnel) {
  // Test-only: simulate a device without a service password.
  const_cast<gm::Config&>(device.dev->app().config()).servicePassword[0] = 0;
  run(request(1, "GET", "/api/status", "", "m1", ""));
  EXPECT_EQ(out.status, 403);
  EXPECT_NE(std::string(out.payload).find("no_password"), std::string::npos);
  EXPECT_NE(std::string(out.payload).find("\"mac\":\"\""), std::string::npos);
}
TEST_F(TunnelTest, SuccessResetsFailureCounter) {
  auto bad = request(1, "GET", "/api/status", "", "m1", "wrong");
  for (int i = 0; i < 4; ++i) run(bad);
  run(request());
  ASSERT_EQ(out.status, 200);
  for (int i = 0; i < 4; ++i) {
    run(bad);
    EXPECT_EQ(out.status, 401);
  }
  run(request(2));
  EXPECT_EQ(out.status, 200);
}
TEST_F(TunnelTest, ReplayCacheStaleAndSessionReset) {
  auto req = request(2, "POST", "/api/config", "name=Once");
  run(req);
  ASSERT_EQ(out.status, 200);
  std::string cached = out.payload;
  tunnel.setBusy(true);
  run(req);
  EXPECT_EQ(cached, out.payload);
  tunnel.setBusy(false);
  run(request(1));
  EXPECT_EQ(out.status, 409);
  checkMac();
  EXPECT_NE(bodyText().find("\"last_seq\":2"), std::string::npos);
  run(request(2, "GET", "/api/status", "", "other"), "other");
  EXPECT_EQ(out.status, 409);
  checkMac(2, "other");
  tunnel.beginSession(nonce, "gm-test");
  run(request());
  EXPECT_EQ(out.status, 200);
}
TEST_F(TunnelTest, RetainedNeverAnswers) {
  auto req = request();
  EXPECT_FALSE(tunnel.handle("m1", req.data(), req.size(), true, 0, &out));
  EXPECT_FALSE(tunnel.handle("!", "x", 1, true, 0, &out));
  run(request());
  EXPECT_EQ(out.status, 200);
}
TEST_F(TunnelTest, PathsBusyAndNotFound) {
  run(request(1, "GET", "/api/history/1/2"));
  EXPECT_EQ(out.status, 200);
  checkMac();
  run(request(2, "POST", "/api/ota"));
  EXPECT_EQ(out.status, 501);
  checkMac(2);
  tunnel.setBusy(true);
  run(request(3));
  EXPECT_EQ(out.status, 503);
  checkMac(3);
  tunnel.setBusy(false);
  run(request(4, "GET", "/index.html"));
  EXPECT_EQ(out.status, 404);
  checkMac(4);
  run(request(5, "GET", "/api/status?x=1"));
  EXPECT_EQ(out.status, 400);
}
TEST_F(TunnelTest, BinaryHistoryBase64MacAndCachedDuplicate) {
  const char* path = "/api/history/0/4294967295";
  for (uint64_t seq = 1; seq <= 2; ++seq) {
    if (seq == 2) {
      gm::HistoryRecord record = {};
      record.batteryMv = 3700;
      record.rssi = -67;
      record.centi[0] = gm::kHistoryMissing;
      record.flags = gm::kHistRestart;
      for (uint32_t i = 0; i < 257; ++i) {
        record.epoch = 1800000000 + i * 60;
        record.centi[1] = static_cast<int16_t>(i * 20);
        ASSERT_TRUE(device.dev->app().history().append(record, 0));
      }
    }
    gm::PortalRequest http = {"GET", path, "", "", 0};
    gm::PortalReply response = {};
    ASSERT_TRUE(portal.handle(http, &response));
    ASSERT_EQ(response.status, 200);
    const std::string original(response.body, response.len);
    EXPECT_EQ(original.size(), seq == 1 ? 16u : 6160u);
    auto req = request(seq, "GET", path);
    run(req);
    ASSERT_EQ(out.status, 200);
    checkMac(seq);
    char enc[16], ctype[64];
    ASSERT_TRUE(gm::jsonGetString(out.payload, "enc", enc, sizeof(enc)));
    EXPECT_STREQ(enc, "base64");
    ASSERT_TRUE(gm::jsonGetString(out.payload, "ctype", ctype, sizeof(ctype)));
    EXPECT_STREQ(ctype, "application/octet-stream");
    std::vector<char> body(9000);
    ASSERT_TRUE(
        gm::jsonGetString(out.payload, "body", body.data(), body.size()));
    const size_t encodedLen = strlen(body.data());
    EXPECT_EQ(encodedLen, seq == 1 ? 24u : 8216u);
    EXPECT_EQ(std::string(body.data()).substr(encodedLen - 2), "==");
    EXPECT_EQ(std::string(body.data())
                  .find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmno"
                                     "pqrstuvwxyz0123456789+/="),
              std::string::npos);
    std::vector<uint8_t> decoded(original.size());
    ASSERT_EQ(gm::base64urlDecode(body.data(), encodedLen, decoded.data(),
                                  decoded.size()),
              static_cast<int>(original.size()));
    gm::gm2StreamXor(gm2test::keys("test-password", "gm-test").enc.data(),
                     "gm-test", nonce, gm2test::cid, seq, "m1", "res",
                     gm2test::field(out.payload, "iv").c_str(), decoded.data(),
                     decoded.size());
    EXPECT_EQ(std::string(decoded.begin(), decoded.end()), original);
    const std::string cached(out.payload, out.len);
    tunnel.setBusy(true);
    run(req);
    EXPECT_EQ(std::string(out.payload, out.len), cached);
    ASSERT_TRUE(gm::jsonGetString(out.payload, "enc", enc, sizeof(enc)));
    EXPECT_STREQ(enc, "base64");
    tunnel.setBusy(false);
  }
}
TEST_F(TunnelTest, BinaryHistoryOverflowReturnsSignedJsonAndCachesIt) {
  std::unique_ptr<gm::PortalTunnel> small(
      new gm::PortalTunnel(portal, device.dev->app(), 180));
  small->beginSession(nonce, "gm-test");
  auto req = request(1, "GET", "/api/history/0/4294967295");
  ASSERT_TRUE(small->handle("m1", req.data(), req.size(), false, 0, &out));
  ASSERT_EQ(out.status, 413);
  checkMac();
  EXPECT_NE(bodyText().find("too_large"), std::string::npos);
  EXPECT_EQ(std::string(out.payload).find("\"enc\""), std::string::npos);
  const std::string cached(out.payload, out.len);
  ASSERT_TRUE(small->handle("m1", req.data(), req.size(), false, 0, &out));
  EXPECT_EQ(std::string(out.payload, out.len), cached);
}
TEST_F(TunnelTest, TooLargeAndRestart) {
  gm::PortalTunnel small(portal, device.dev->app(), 300);
  small.beginSession(nonce, "gm-test");
  auto req = request();
  ASSERT_TRUE(small.handle("m1", req.data(), req.size(), false, 0, &out));
  EXPECT_EQ(out.status, 413);
  checkMac();
  run(request(1, "POST", "/api/restart"));
  EXPECT_EQ(out.status, 200);
  EXPECT_TRUE(out.restart);
  checkMac();
  std::string cached = out.payload;
  run(request(1, "POST", "/api/restart"));
  EXPECT_EQ(cached, out.payload);
  EXPECT_TRUE(out.restart);
}
TEST_F(TunnelTest, UnicodeEscapesAndEscapedResponse) {
  auto req = request(1, "POST", "/api/config", "name=A%22B%5CC");
  auto pos = req.find("/api/config");
  req.replace(pos, 11, "\\u002fapi/config");
  run(req);
  ASSERT_EQ(out.status, 200);
  run(request(2, "GET", "/api/config"));
  ASSERT_EQ(out.status, 200);
  checkMac(2);
  EXPECT_NE(bodyText().find("A\\\"B\\\\C"), std::string::npos);
  auto body = request(3, "POST", "/api/config", "name=Unicode");
  pos = body.find("\"body\":");
  ASSERT_NE(pos, std::string::npos);
  // Escaped ASCII in the ciphertext string has identical canonical bytes.
  pos += 8;
  const char first = body[pos];
  char escape[7];
  snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(first));
  body.replace(pos, 1, escape);
  run(body);
  EXPECT_EQ(out.status, 200);
  EXPECT_STREQ(device.dev->app().config().deviceName, "Unicode");
}
TEST_F(TunnelTest, FieldOrderUnicodeAndLengthDelimitedInput) {
  auto original =
      request(1, "POST", "/api/config", "name=Unicode&extra=Zażółć");
  const auto macPos = original.find("\"mac\":");
  const std::string macField =
      original.substr(macPos, original.size() - macPos - 1);
  std::string reordered =
      "{ " + macField +
      ",\"body\":" + gm2test::quote(gm2test::field(original.c_str(), "body")) +
      ",\"cid\":\"0123456789abcdef\",\"iv\":"
      "\"abcdef0123456789abcdef0123456789\","
      "\"path\":\"\\u002fapi/config\",\"method\":\"POST\",\"seq\":1,\"v\":2 }";
  std::vector<char> bytes(reordered.begin(), reordered.end());
  ASSERT_TRUE(tunnel.handle("m1", bytes.data(), bytes.size(), false, 0, &out));
  EXPECT_EQ(out.status, 200);
  checkMac();
  EXPECT_STREQ(device.dev->app().config().deviceName, "Unicode");
  auto emoji = request(2, "POST", "/api/config", "extra=😀");
  run(emoji);
  EXPECT_EQ(out.status, 200);
  checkMac(2);
}
TEST_F(TunnelTest, InvalidPortalUtf8IsSafeAndVerifiable) {
  // Simulate a legacy/corrupt stored value that bypassed config validation.
  auto& config = const_cast<gm::Config&>(device.dev->app().config());
  strcpy(config.deviceName, "bad\xff");
  gm::PortalRequest req = {"GET", "/api/config", "test-password", "", 0};
  gm::PortalReply reply = {};
  ASSERT_TRUE(portal.handle(req, &reply));
  const std::string original(reply.body, reply.len);
  ASSERT_NE(original.find('\xff'), std::string::npos);
  run(request(1, "GET", "/api/config"));
  ASSERT_EQ(out.status, 200);
  checkMac();
  EXPECT_EQ(bodyText(), original);
  EXPECT_EQ(std::string(out.payload).find('\xff'), std::string::npos);
}
TEST_F(TunnelTest, StrictMalformedInput) {
  const std::string good = request();
  const std::string invalid[] = {"",
                                 good + "x",
                                 good.substr(0, good.size() - 1),
                                 "{\"v\":1,\"v\":1}",
                                 std::string(4097, ' '),
                                 "{\"v\":01}",
                                 "{\"v\":1e0}",
                                 "{\"v\":1,\"body\":\"\\u0000\"}"};
  for (const auto& s : invalid) {
    run(s);
    EXPECT_EQ(out.status, 400);
  }
  run(good, "bad/mid");
  EXPECT_EQ(out.status, 400);
  auto oversized =
      request(1, "POST", "/api/config", std::string(2049, 'x').c_str());
  run(oversized);
  EXPECT_EQ(out.status, 400);
  auto duplicate = good;
  duplicate.insert(1, "\"v\":1,");
  run(duplicate);
  EXPECT_EQ(out.status, 400);
  run(request());
  EXPECT_EQ(out.status, 200);
}
TEST_F(TunnelTest, CiphertextBitFlipFailsMacWithoutExecuting) {
  auto req = request(1, "POST", "/api/config", "name=Tampered");
  const size_t pos = req.find("\"body\":\"") + 8;
  ASSERT_LT(pos, req.size());
  req[pos] = req[pos] == 'A' ? 'B' : 'A';
  run(req);
  EXPECT_EQ(out.status, 401);
  EXPECT_EQ(bodyText(), "{\"error\":\"bad_mac\"}");
  EXPECT_EQ(gm2test::field(out.payload, "mac"), "");
  EXPECT_STRNE(device.dev->app().config().deviceName, "Tampered");
  run(request());
  EXPECT_EQ(out.status, 200);
}
TEST_F(TunnelTest, IndependentClientsAndRetiredLruReplayProtection) {
  const char* clients[] = {"0000000000000001", "0000000000000002",
                           "0000000000000003", "0000000000000004",
                           "0000000000000005"};
  for (int i = 0; i < 4; ++i) {
    run(request(1, "GET", "/api/status", "", "m1", "test-password",
                clients[i]));
    ASSERT_EQ(out.status, 200);
    checkMac();
  }
  // Refresh client 1's LRU position; client 2, not client 1, is retired.
  run(request(2, "GET", "/api/status", "", "m1", "test-password", clients[0]));
  ASSERT_EQ(out.status, 200);
  run(request(1, "GET", "/api/status", "", "m1", "test-password", clients[4]));
  ASSERT_EQ(out.status, 200);
  for (uint64_t seq : {1ULL, 100ULL}) {
    run(request(seq, "POST", "/api/config", "name=Replay", "m1",
                "test-password", clients[1]));
    ASSERT_EQ(out.status, 409);
    checkMac(seq);
    EXPECT_EQ(bodyText(), "{\"error\":\"client_retired\"}");
  }
  run(request(3, "GET", "/api/status", "", "m1", "test-password", clients[0]));
  EXPECT_EQ(out.status, 200);
  EXPECT_STRNE(device.dev->app().config().deviceName, "Replay");
}
TEST_F(TunnelTest, DuplicateIdentityAndErrorIvsNeverReuseKeystream) {
  run(request(2, "POST", "/api/config", "name=Once"));
  ASSERT_EQ(out.status, 200);
  const std::string cached = out.payload;
  const std::string initialIv = gm2test::field(out.payload, "iv");
  run(request(2, "POST", "/api/config", "name=Twice"));
  ASSERT_EQ(out.status, 409);
  EXPECT_STRNE(device.dev->app().config().deviceName, "Twice");
  const std::string staleIv = gm2test::field(out.payload, "iv");
  EXPECT_GT(staleIv, initialIv);
  run(request(1));
  ASSERT_EQ(out.status, 409);
  EXPECT_GT(gm2test::field(out.payload, "iv"), staleIv);
  // A different signed error does not replace the accepted-request cache.
  run(request(2, "POST", "/api/config", "name=Once"));
  EXPECT_EQ(std::string(out.payload), cached);
  tunnel.setBusy(true);
  run(request(3));
  ASSERT_EQ(out.status, 503);
  const std::string busyIv = gm2test::field(out.payload, "iv");
  run(request(4));
  ASSERT_EQ(out.status, 503);
  EXPECT_GT(gm2test::field(out.payload, "iv"), busyIv);
}
TEST_F(TunnelTest, KeyRefreshPreservesReplayAndCounterAndInvalidatesCache) {
  run(request(5, "POST", "/api/config", "svc_pass=changed-password"));
  ASSERT_EQ(out.status, 200);
  checkMac(5);
  const std::string oldIv = gm2test::field(out.payload, "iv");
  EXPECT_TRUE(tunnel.refreshKeys());
  EXPECT_FALSE(tunnel.refreshKeys());
  run(request(5, "POST", "/api/config", "svc_pass=changed-password", "m1",
              "changed-password"));
  ASSERT_EQ(out.status, 409);
  checkMac(5, "m1", "changed-password");
  EXPECT_GT(gm2test::field(out.payload, "iv"), oldIv);
  run(request(6, "GET", "/api/status", "", "m1", "changed-password"));
  ASSERT_EQ(out.status, 200);
  checkMac(6, "m1", "changed-password");
}
TEST_F(TunnelTest, EvictedDuplicateDoesNotSuggestReexecutingMutation) {
  gm::HistoryRecord record = {};
  for (uint32_t i = 0; i < 257; ++i) {
    record.epoch = 1800000000 + i * 60;
    record.centi[1] = static_cast<int16_t>(i * 20);
    ASSERT_TRUE(device.dev->app().history().append(record, 0));
  }
  const std::string original = request(1, "POST", "/api/config", "name=Once");
  run(original);
  ASSERT_EQ(out.status, 200);
  run(request(1, "POST", "/api/config", "name=Later", "m1", "test-password",
              "ffffffffffffffff"));
  ASSERT_EQ(out.status, 200);
  run(request(2, "GET", "/api/history/0/4294967295", "", "m1", "test-password",
              "ffffffffffffffff"));
  ASSERT_EQ(out.status, 200);
  ASSERT_GT(out.len, static_cast<size_t>(gm::PortalTunnel::kMaxCache / 2));
  const std::string largeRequest =
      request(1, "GET", "/api/history/0/4294967295", "", "m1", "test-password",
              "eeeeeeeeeeeeeeee");
  run(largeRequest);
  ASSERT_EQ(out.status, 200);
  const std::string largeCached = out.payload;
  // Restore request metadata for decryption, then retry the exact publication.
  request(1, "POST", "/api/config", "name=Once");
  run(original);
  ASSERT_EQ(out.status, 409);
  checkMac();
  EXPECT_EQ(bodyText(), "{\"error\":\"duplicate_expired\"}");
  EXPECT_EQ(bodyText().find("last_seq"), std::string::npos);
  EXPECT_STREQ(device.dev->app().config().deviceName, "Later");
  run(largeRequest);
  EXPECT_EQ(std::string(out.payload), largeCached);
}
TEST_F(TunnelTest, SeveralClientsRetainExactEnvelopesAndPoolCompacts) {
  const std::string a = request(1, "POST", "/api/restart");
  run(a);
  ASSERT_EQ(out.status, 200);
  ASSERT_TRUE(out.restart);
  const std::string aCached = out.payload;
  const std::string b = request(1, "GET", "/missing", "", "m1", "test-password",
                                "ffffffffffffffff");
  run(b);
  ASSERT_EQ(out.status, 404);
  ASSERT_FALSE(out.restart);
  const std::string bCached = out.payload;
  run(a);
  EXPECT_EQ(std::string(out.payload), aCached);
  EXPECT_EQ(out.status, 200);
  EXPECT_TRUE(out.restart);
  run(b);
  EXPECT_EQ(std::string(out.payload), bCached);
  EXPECT_EQ(out.status, 404);
  EXPECT_FALSE(out.restart);
  const std::string c = request(1, "GET", "/api/status", "", "m1",
                                "test-password", "eeeeeeeeeeeeeeee");
  run(c);
  ASSERT_EQ(out.status, 200);
  const std::string cCached = out.payload;
  // Replacing B's middle entry must compact without damaging A or C.
  const std::string b2 = request(2, "GET", "/api/status", "", "m1",
                                 "test-password", "ffffffffffffffff");
  run(b2);
  ASSERT_EQ(out.status, 200);
  const std::string b2Cached = out.payload;
  const std::string d = request(1, "GET", "/api/status", "", "m1",
                                "test-password", "dddddddddddddddd");
  run(d);
  ASSERT_EQ(out.status, 200);
  const std::string dCached = out.payload;
  run(a);
  EXPECT_EQ(std::string(out.payload), aCached);
  run(b2);
  EXPECT_EQ(std::string(out.payload), b2Cached);
  run(c);
  EXPECT_EQ(std::string(out.payload), cCached);
  run(d);
  EXPECT_EQ(std::string(out.payload), dCached);
}
TEST_F(TunnelTest, StatusSignatureMatchesSharedCanonicalEscapingVector) {
  std::ifstream file(std::string(GM_TEST_PORTAL_DIR) +
                     "/../tests/data/gm2-vectors.json");
  ASSERT_TRUE(file.good());
  // Not istreambuf_iterator: GCC 13 -O2 reports a false -Wnull-dereference.
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string json = buffer.str();
  const std::string password = gm2test::field(json.c_str(), "password");
  strcpy(const_cast<gm::Config&>(device.dev->app().config()).servicePassword,
         password.c_str());
  ASSERT_TRUE(tunnel.refreshKeys());
  const size_t pos = json.find("\"status\": {");
  ASSERT_NE(pos, std::string::npos);
  const char* fields = json.c_str() + json.find('{', pos);
  char mac[65];
  tunnel.statusMac("2.0.0", "Silo \"\\\b\f\nż😀", 1800003600, 3600, 7200,
                   "finish", "9.9.9-test", "ok", mac);
  EXPECT_EQ(std::string(mac), gm2test::field(fields, "mac"));
  const std::string text = gm2test::field(fields, "text");
  EXPECT_EQ(std::string(mac),
            gm2test::hmac(gm2test::keys(password.c_str(), "gm-test").mac.data(),
                          text));
  tunnel.statusMac("2.0.0", "name", 0, 60, 120, "", "", "", mac);
  const std::string nullText =
      std::string("gm2status\n[2,\"gm-test\",\"online\",\"") + nonce +
      "\",\"2.0.0\",\"name\",0,60,120,10000,null]";
  EXPECT_EQ(std::string(mac),
            gm2test::hmac(gm2test::keys(password.c_str(), "gm-test").mac.data(),
                          nullText));
  const_cast<gm::Config&>(device.dev->app().config()).servicePassword[0] = 0;
  ASSERT_TRUE(tunnel.refreshKeys());
  tunnel.statusMac("fw", "name", 0, 0, 0, "", "", "", mac);
  EXPECT_STREQ(mac, "");
}
TEST_F(TunnelTest, RestartFlagIsAuthenticated) {
  run(request(1, "POST", "/api/restart"));
  ASSERT_TRUE(out.restart);
  checkMac();
  std::string modified = out.payload;
  const size_t pos = modified.find("\"restart\":true");
  ASSERT_NE(pos, std::string::npos);
  modified.replace(pos, 14, "\"restart\":false");
  EXPECT_FALSE(gm2test::validMac(modified.c_str(), "gm-test", nonce, 1, "m1"));
}
TEST_F(TunnelTest, SafeIntegerAndCanonicalLowerHexAndBase64) {
  for (uint64_t seq : {0ULL, 9007199254740992ULL}) {
    run(request(seq));
    EXPECT_EQ(out.status, 400);
  }
  run(request(9007199254740991ULL));
  ASSERT_EQ(out.status, 200);
  checkMac(9007199254740991ULL);
  const std::string good = request(9007199254740991ULL);
  for (const char* field : {"cid", "iv"}) {
    std::string modified = good;
    size_t pos =
        modified.find(std::string("\"") + field + "\":\"") + strlen(field) + 4;
    modified[pos] = 'A';
    run(modified);
    EXPECT_EQ(out.status, 400);
  }
  std::string badBase64 = request(1, "POST", "/api/config", "a");
  const size_t pos = badBase64.find("\"body\":\"") + 8;
  badBase64[pos] = '_';
  run(badBase64);
  EXPECT_EQ(out.status, 400);
}
}  // namespace
