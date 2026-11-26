// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

#include "support/fake_image.h"
#include "support/gm2.h"
#include "support/json_check.h"
#include "support/ota_upload.h"
#include "support/test_device.h"
#include "util/hmac_sha256.h"
#include "util/json_read.h"

namespace {
class OtaTunnelTest : public ::testing::TestWithParam<bool> {
 protected:
  TestDevice device{"ota_tunnel"};
  std::string nonce = "0123456789abcdef0123456789abcdef";
  uint64_t seq = 0;
  gm::TunnelOut out{};
  std::string response, body;
  void SetUp() override {
    ASSERT_TRUE(device.started);
    device.dev->beginRemoteSession(nonce.c_str(), "gm-test");
  }
  void send(const char* method, const char* path, const std::string& data = "",
            const char* mid = "upload") {
    ++seq;
    const std::string request = gm2test::request("gm-test", nonce.c_str(), seq,
                                                 method, path, data, mid);
    ASSERT_LE(request.size(), 4096u);
    ASSERT_TRUE(
        device.dev->handleTunnel(mid, request.c_str(), false, &out, &response));
    EXPECT_EQ(jsoncheck::error(response), "");
    EXPECT_TRUE(gm2test::validMac(response.c_str(), "gm-test", nonce.c_str(),
                                  seq, mid));
    body = gm2test::plaintext(response.c_str(), "gm-test", nonce.c_str(), seq,
                              mid);
  }
};

TEST_P(OtaTunnelTest, FullImageRestartStatusAndRollback) {
  const auto image = fakeImage(1350001);
  const std::string hash = imageHash(image);
  send("POST", "/api/ota");
  ASSERT_EQ(out.status, 501);
  if (GetParam()) {
    send("POST", "/api/sim", "action=ota_fail_boot&arg=on");
    ASSERT_EQ(out.status, 200);
  }
  send("POST", "/api/ota/begin",
       "size=" + std::to_string(image.size()) + "&sha256=" + hash +
           "&name=tunnel.bin");
  ASSERT_EQ(out.status, 200) << body;
  EXPECT_NE(body.find("\"chunk\":1440"), std::string::npos);
  for (size_t off = 0; off < image.size();) {
    const size_t len = std::min<size_t>(1440, image.size() - off);
    const std::string form = "off=" + std::to_string(off) +
                             "&d=" + encodeChunk(image.data() + off, len);
    ASSERT_LE(form.size(), static_cast<size_t>(gm::kPortalMaxBody));
    send("POST", "/api/ota/chunk", form);
    ASSERT_EQ(out.status, 200) << body;
    EXPECT_FALSE(out.restart);
    if (off == 0) {
      // A retry with a fresh seq/mid also needs chunk-level idempotency.
      send("POST", "/api/ota/chunk", form, "retry");
      ASSERT_EQ(out.status, 200);
      send("GET", "/api/ota");
      ASSERT_EQ(out.status, 200);
      EXPECT_NE(body.find("\"received\":1440"), std::string::npos);
      EXPECT_NE(body.find("\"expected\":1350001"), std::string::npos);
    }
    off += len;
  }
  send("POST", "/api/ota/finish", "sha256=" + hash, "finish");
  ASSERT_EQ(out.status, 200) << body;
  EXPECT_TRUE(out.restart);
  EXPECT_TRUE(device.dev->restartPending());
  EXPECT_NE(body.find("\"version\":\"9.9.9-test\""), std::string::npos);
  EXPECT_NE(body.find("\"bytes\":1350001"), std::string::npos);
  EXPECT_NE(body.find("\"restarting\":true"), std::string::npos);
  std::ifstream slot(device.dir + "/ota_1.bin", std::ios::binary);
  const std::vector<uint8_t> written{std::istreambuf_iterator<char>(slot),
                                     std::istreambuf_iterator<char>()};
  EXPECT_EQ(written, image);
  device.dev->tick();
  EXPECT_FALSE(device.dev->restartPending());
  EXPECT_STREQ(device.dev->ota().runningSlot(), GetParam() ? "ota_0" : "ota_1");
  const std::string status = device.dev->remoteStatus();
  EXPECT_EQ(jsoncheck::error(status), "");
  EXPECT_TRUE(gm2test::validStatus(status.c_str(), "gm-test"));
  EXPECT_NE(status.find("\"mid\":\"finish\""), std::string::npos);
  EXPECT_NE(
      status.find(GetParam() ? "\"result\":\"failed\"" : "\"result\":\"ok\""),
      std::string::npos);
  EXPECT_NE(status.find("\"fw\":\"" +
                        std::string(GetParam() ? GM_FW_VERSION : "9.9.9-test") +
                        "\""),
            std::string::npos);
  char fresh[33];
  ASSERT_TRUE(gm::jsonGetString(status.c_str(), "nonce", fresh, sizeof(fresh)));
  EXPECT_NE(nonce, fresh);
  nonce = fresh;
  seq = 0;
  send("GET", "/api/ota");
  EXPECT_EQ(out.status, 200);
  if (GetParam()) {
    EXPECT_EQ(device.dev->ota().rejectedVersion(), "9.9.9-test");
  }
}
TEST_P(OtaTunnelTest, OrdinaryRestartReportsSignedSuccessfulOperation) {
  send("POST", "/api/restart", "", "restart");
  ASSERT_EQ(out.status, 200);
  ASSERT_TRUE(out.restart);
  device.dev->tick();
  const std::string status = device.dev->remoteStatus();
  EXPECT_TRUE(gm2test::validStatus(status.c_str(), "gm-test"));
  EXPECT_NE(status.find("\"mid\":\"restart\""), std::string::npos);
  EXPECT_NE(status.find("\"result\":\"ok\""), std::string::npos);
  EXPECT_NE(gm2test::field(status.c_str(), "nonce"), nonce);
  std::string tampered = status;
  size_t pos = tampered.find("\"result\":\"ok\"");
  ASSERT_NE(pos, std::string::npos);
  tampered.replace(pos, strlen("\"result\":\"ok\""), "\"result\":\"failed\"");
  EXPECT_FALSE(gm2test::validStatus(tampered.c_str(), "gm-test"));
  tampered = status;
  pos = tampered.find("\"name\":\"");
  ASSERT_NE(pos, std::string::npos);
  tampered.insert(pos + strlen("\"name\":\""), "tampered ");
  EXPECT_FALSE(gm2test::validStatus(tampered.c_str(), "gm-test"));
}

INSTANTIATE_TEST_SUITE_P(Boot, OtaTunnelTest, ::testing::Bool());
}  // namespace
