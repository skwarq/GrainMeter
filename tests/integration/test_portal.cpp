// SPDX-License-Identifier: MIT
// Portal integration: the real PC HTTP server (cpp-httplib) in front of the
// shared portal logic, exercised over TCP with an HTTP client.
#include <gtest/gtest.h>
#include <httplib.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "app/portal_tunnel.h"
#include "platform/web_server.h"
#include "support/fake_image.h"
#include "support/gm2.h"
#include "support/json_check.h"
#include "support/ota_upload.h"
#include "support/test_device.h"
#include "util/base64url.h"
#include "util/hmac_sha256.h"
#include "util/json_read.h"

#ifndef GM_TEST_PORTAL_DIR
#define GM_TEST_PORTAL_DIR "portal"
#endif

class PortalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(t.started);
    web.reset(new gmpc::WebServer(*t.dev, GM_TEST_PORTAL_DIR));
    port = web->start("127.0.0.1", 0);
    ASSERT_GT(port, 0);
    cli.reset(new httplib::Client("127.0.0.1", port));
    cli->set_read_timeout(30, 0);
  }
  void TearDown() override { web->stop(); }

  httplib::Result post(const std::string& path, const std::string& body,
                       bool auth = true) {
    httplib::Headers h;
    if (auth) h.emplace("X-GM-Password", "test-password");
    return cli->Post(path, h, body, "application/x-www-form-urlencoded");
  }

  TestDevice t{"portal"};
  std::unique_ptr<gmpc::WebServer> web;
  std::unique_ptr<httplib::Client> cli;
  int port = 0;
};

TEST_F(PortalTest, ServesPortalPage) {
  auto r = cli->Get("/");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(r->body.find("<"), std::string::npos);
  EXPECT_EQ(r->get_header_value("X-Content-Type-Options"), "nosniff");
}

TEST_F(PortalTest, StatusIsJsonWithoutSecrets) {
  auto r = cli->Get("/api/status");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200);
  EXPECT_EQ(r->get_header_value("Content-Type"), "application/json");
  EXPECT_NE(r->body.find("\"platform\":\"pc-sim\""), std::string::npos);
  EXPECT_NE(r->body.find("\"battery_forecast\":{"), std::string::npos);
  EXPECT_EQ(jsoncheck::error(r->body), "");
  auto d = cli->Get("/api/diag");
  ASSERT_TRUE(d);
  EXPECT_EQ(jsoncheck::error(d->body), "");
  EXPECT_NE(d->body.find("\"battery_forecast\":{"), std::string::npos);
  EXPECT_EQ(d->body.find("test-password"), std::string::npos);
  EXPECT_EQ(d->body.find("SIMULATEDKEY"), std::string::npos);
  EXPECT_EQ(d->body.find("simulated-wifi-pass"), std::string::npos);
}

TEST_F(PortalTest, WritesNeedPassword) {
  auto r = post("/api/scan", "", false);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 401);
  httplib::Headers wrong = {{"X-GM-Password", "nope"}};
  r = cli->Post("/api/config", wrong, "measure_s=60",
                "application/x-www-form-urlencoded");
  EXPECT_EQ(r->status, 401);
  r = post("/api/login", "");
  EXPECT_EQ(r->status, 200);
}

TEST_F(PortalTest, LogPreviewReadOnlyCursorLossAndInvalidPaths) {
  auto r = cli->Get("/api/logs/0");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200);
  EXPECT_EQ(jsoncheck::error(r->body), "");
  EXPECT_NE(r->body.find("pc: simulated log preview"), std::string::npos);
  uint32_t next = 0;
  ASSERT_TRUE(gm::jsonGetUint(r->body.c_str(), "next", &next));
  for (int i = 0; i < 20; ++i) ASSERT_EQ(cli->Get("/api/logs/0")->status, 200);
  r = cli->Get("/api/logs/" + std::to_string(next));
  ASSERT_TRUE(r);
  uint32_t lost = 0;
  ASSERT_TRUE(gm::jsonGetUint(r->body.c_str(), "lost", &lost));
  EXPECT_GT(lost, 0u);
  EXPECT_LT(r->body.size(), 8192u);
  r = cli->Get("/api/logs/999999");
  ASSERT_TRUE(r);
  EXPECT_NE(r->body.find("\"lines\":[]"), std::string::npos);
  for (const char* path : {"/api/logs/", "/api/logs/-1", "/api/logs/1x",
                           "/api/logs/1/2", "/api/logs/18446744073709551616"}) {
    r = cli->Get(path);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400) << path;
  }
}

TEST_F(PortalTest, ChunkedOtaPermissionsParsingOffsetAndTimeout) {
  const auto image = fakeImage(5000);
  const std::string hash = imageHash(image);
  const std::string begin = "size=5000&sha256=" + hash + "&name=http.bin";
  for (const char* path : {"/api/ota/begin", "/api/ota/chunk",
                           "/api/ota/finish", "/api/ota/abort"}) {
    auto r = post(path, "", false);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 401);
  }
  for (const char* size : {"-1", "+5000", "5000x", "184467440737095516160"}) {
    auto r = post("/api/ota/begin",
                  "size=" + std::string(size) + "&sha256=" + hash + "&name=x");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, 400);
  }
  auto r = post("/api/ota/begin", begin);
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200) << r->body;
  r = post("/api/ota/chunk", "off=0&d=invalid!");
  EXPECT_EQ(r->status, 400);
  r = post("/api/ota/chunk", "off=0&d=" + encodeChunk(image.data(), 1441));
  EXPECT_EQ(r->status, 400);
  r = post("/api/ota/chunk", "off=1440&d=" + encodeChunk(image.data(), 1440));
  EXPECT_EQ(r->status, 409);
  EXPECT_NE(r->body.find("\"error\":\"offset\""), std::string::npos);
  EXPECT_NE(r->body.find("\"next\":0"), std::string::npos);
  r = post("/api/ota/chunk", "off=0&d=" + encodeChunk(image.data(), 1440));
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(cli->Get("/api/ota")->body.find("\"received\":1440"),
            std::string::npos);
  t.dev->clock().advanceMs(120000);
  t.dev->tick();  // timeout must run even when OTA normally suppresses tick
  r = cli->Get("/api/ota");
  EXPECT_NE(r->body.find("upload timeout"), std::string::npos);
  EXPECT_FALSE(t.dev->ota().pending());
  r = post("/api/ota/begin", begin);
  EXPECT_EQ(r->status, 200);
  r = post("/api/ota/abort", "");
  EXPECT_EQ(r->status, 200);
  EXPECT_FALSE(t.dev->app().ota().busy());
}

TEST_F(PortalTest, ChunkedOtaCriticalAndHttpMutualExclusion) {
  const auto image = fakeImage(5000);
  const std::string begin = "size=5000&sha256=" + imageHash(image) + "&name=x";
  post("/api/sim", "action=battery&value=2900");
  auto r = post("/api/ota/begin", begin);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 503);
  EXPECT_NE(r->body.find("low_battery"), std::string::npos);
  post("/api/sim", "action=battery&value=3900");
  gm::PortalReply reply{};
  std::string body;
  ASSERT_TRUE(t.dev->otaStart("test-password", image.size(), gm::OtaBody::Raw,
                              "http.bin", &reply, &body));
  r = post("/api/ota/begin", begin);
  EXPECT_EQ(r->status, 409);
  EXPECT_NE(r->body.find("\"error\":\"busy\""), std::string::npos);
  post("/api/ota/abort", "");
  EXPECT_TRUE(t.dev->app().ota().busy());  // chunk abort cannot cancel HTTP
  t.dev->otaAbort("test abort");
  r = post("/api/ota/begin", begin);
  ASSERT_EQ(r->status, 200);
  EXPECT_FALSE(t.dev->otaStart("test-password", image.size(), gm::OtaBody::Raw,
                               "http.bin", &reply, &body));
  EXPECT_EQ(reply.status, 409);
  post("/api/ota/abort", "");
}

TEST_F(PortalTest, ChunkedOtaCompletesOverHttpWithStandardBase64AndPadding) {
  auto image = fakeImage(5000);
  image.back() = 0xFF;
  image[image.size() - 2] = 0xFB;
  const std::string hash = imageHash(image);
  auto r =
      post("/api/ota/begin", "size=5000&sha256=" + hash + "&name=http.bin");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200);
  for (size_t off = 0; off < image.size();) {
    const size_t len = std::min<size_t>(1440, image.size() - off);
    const std::string encoded = encodeChunk(image.data() + off, len);
    std::string standard;
    for (const char c : encoded) {
      standard += c == '-' ? "%2B" : c == '_' ? "%2F" : std::string(1, c);
    }
    if (len % 3) standard.append(3 - len % 3, '=');
    r = post("/api/ota/chunk", "off=" + std::to_string(off) + "&d=" + standard);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->status, 200) << r->body;
    off += len;
  }
  r = post("/api/ota/finish", "sha256=" + hash);
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200) << r->body;
  EXPECT_NE(r->body.find("\"restarting\":true"), std::string::npos);
  EXPECT_TRUE(t.dev->restartPending());
  t.dev->tick();
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_1");
}

TEST_F(PortalTest, RemoteExtensionRequiresPasswordAndSession) {
  auto r = post("/api/remote/extend", "minutes=10", false);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 401);
  r = post("/api/remote/extend", "minutes=10");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 409);
  EXPECT_NE(r->body.find("no_session"), std::string::npos);
  r = post("/api/config", "remote_idle_min=17");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(cli->Get("/api/config")->body.find("\"remote_idle_min\":17"),
            std::string::npos);
  t.dev->restart("config reload");
  EXPECT_EQ(t.dev->app().config().remoteIdleMin, 17u);
}

TEST_F(PortalTest, SimulatorSessionExtendsThroughPortalTunnelAfterDeadline) {
  auto r = post("/api/config", "remote_url=mqtts://broker&remote_idle_min=60");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200);
  r = post("/api/sim", "action=remote_start&minutes=10");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200);
  t.dev->clock().advanceMs(601000);
  const char* nonce = "0123456789abcdef0123456789abcdef";
  const char* path = "/api/remote/extend";
  const char* body = "minutes=1";
  const std::string request =
      gm2test::request("gm-test", nonce, 1, "POST", path, body, "extend");
  std::lock_guard<std::mutex> lock(t.dev->mutex());
  gm::Portal portal(t.dev->app());
  gm::PortalTunnel tunnel(portal, t.dev->app());
  tunnel.beginSession(nonce, "gm-test");
  gm::TunnelOut out{};
  ASSERT_TRUE(tunnel.handle("extend", request.data(), request.size(), false,
                            t.dev->clock().monoS(), &out));
  ASSERT_EQ(out.status, 200);
  EXPECT_TRUE(gm2test::validMac(out.payload, "gm-test", nonce, 1, "extend"));
  const std::string replyBody =
      gm2test::plaintext(out.payload, "gm-test", nonce, 1, "extend");
  EXPECT_NE(replyBody.find("\"remaining_s\":60"), std::string::npos);
  EXPECT_NE(std::string(replyBody).find("\"until\":0"), std::string::npos);
  EXPECT_EQ(t.dev->info().remainingS, 60u);
}

TEST_F(PortalTest, LockoutAfterRepeatedFailures) {
  httplib::Headers wrong = {{"X-GM-Password", "nope"}};
  for (int i = 0; i < 5; ++i) cli->Post("/api/login", wrong, "", "text/plain");
  auto r = post("/api/login", "");  // correct password, but locked
  EXPECT_EQ(r->status, 401);
  t.dev->clock().advanceMs(61000);
  r = post("/api/login", "");
  EXPECT_EQ(r->status, 200);
}

TEST_F(PortalTest, ConfigScanMapMeasure) {
  auto r = post("/api/config", "measure_s=1&upload_s=3600");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 400);  // below PC minimum (5 s)
  EXPECT_NE(r->body.find("measure_s"), std::string::npos);
  r = post("/api/config", "measure_s=600&upload_s=2400&name=Silo+3");
  EXPECT_EQ(r->status, 200) << r->body;
  EXPECT_NE(cli->Get("/api/config")->body.find("\"samples_per_upload\":4"),
            std::string::npos);

  r = post("/api/scan", "");
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(r->body.find("\"family\":\"0x28\""), std::string::npos);

  const gm::Rom rom = gmpc::SimProbe::makeRom(0x28, 0x1E8564FF);
  char hex[17];
  gm::romToHex(rom, hex, sizeof(hex));
  r = post("/api/map", std::string("rom=") + hex + "&point=4&height_cm=250");
  EXPECT_EQ(r->status, 200) << r->body;
  r = post("/api/map", std::string("rom=") + hex + "&point=9");
  EXPECT_EQ(r->status, 400);
  r = post("/api/measure", "");
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(r->body.find(std::string("\"point\":4,\"rom\":\"") + hex +
                         "\",\"height_cm\":250,\"status\":\"ok\""),
            std::string::npos)
      << r->body;
  r = post("/api/unmap", std::string("rom=") + hex);
  EXPECT_EQ(r->status, 200);
  r = post("/api/automap", "");
  EXPECT_NE(r->body.find("8 sensor(s)"), std::string::npos);
}

TEST_F(PortalTest, SimulatedOutageAndRecoveryThroughApi) {
  post("/api/automap", "");
  post("/api/measure", "store=1");
  auto r = post("/api/sim", "action=net&arg=down");
  EXPECT_EQ(r->status, 200);
  r = post("/api/upload", "");
  EXPECT_NE(r->body.find("\"ok\":false"), std::string::npos);
  EXPECT_NE(cli->Get("/api/status")->body.find("\"count\":1"),
            std::string::npos);  // kept
  post("/api/sim", "action=net&arg=up");
  r = post("/api/upload", "");
  EXPECT_NE(r->body.find("\"ok\":true"), std::string::npos) << r->body;
  EXPECT_NE(r->body.find("\"samples_confirmed\":1"), std::string::npos);
  r = post("/api/sim", "action=explode");
  EXPECT_EQ(r->status, 400);
}

TEST_F(PortalTest, OtaMultipartUploadAndSimulatedRestart) {
  const auto img = fakeImage(200000);
  const std::string body =
      multipartBody("----gmBoundary42", img, "anything.bin");
  httplib::Headers h = {{"X-GM-Password", "test-password"}};
  auto r = cli->Post("/api/ota", h, body,
                     "multipart/form-data; boundary=----gmBoundary42");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200) << r->body;
  EXPECT_NE(r->body.find("9.9.9-test"), std::string::npos);
  EXPECT_TRUE(t.dev->restartPending());
  t.dev->tick();  // performs the simulated restart
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_1");
  EXPECT_NE(cli->Get("/api/status")->body.find("software restart (OTA)"),
            std::string::npos);
}

TEST_F(PortalTest, OtaRawUploadRejectsForeignImage) {
  const auto img = fakeImage(50000, 0x0002);  // ESP32-S2 image
  httplib::Headers h = {{"X-GM-Password", "test-password"},
                        {"X-File-Name", "other_device.bin"}};
  auto r = cli->Post("/api/ota", h, reinterpret_cast<const char*>(img.data()),
                     img.size(), "application/octet-stream");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 415);
  EXPECT_NE(r->body.find("different chip"), std::string::npos);
  EXPECT_FALSE(t.dev->restartPending());
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
}

TEST_F(PortalTest, OtaRequiresPasswordAndCompleteUpload) {
  const auto img = fakeImage(50000);
  auto r = cli->Post("/api/ota", reinterpret_cast<const char*>(img.data()),
                     img.size(), "application/octet-stream");
  EXPECT_EQ(r->status, 401);
  // multipart without terminating delimiter = interrupted upload
  std::string body = multipartBody("bnd", img, "fw.bin");
  body.resize(body.size() - 12);
  httplib::Headers h = {{"X-GM-Password", "test-password"}};
  r = cli->Post("/api/ota", h, body, "multipart/form-data; boundary=bnd");
  EXPECT_EQ(r->status, 400);
  EXPECT_FALSE(t.dev->ota().pending());
  EXPECT_NE(cli->Get("/api/ota")->body.find("\"state\":\"failed\""),
            std::string::npos);
}

TEST_F(PortalTest, LocalHistoryForCharts) {
  post("/api/automap", "");
  auto info = cli->Get("/api/history");
  ASSERT_TRUE(info);
  EXPECT_NE(info->body.find("\"available\":true"), std::string::npos);
  EXPECT_NE(info->body.find("\"records\":0"), std::string::npos);
  // empty history (no clock yet) is a valid, empty export
  const char* all = "/api/history/0/4294967295";
  auto r = cli->Get(all);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200);
  EXPECT_EQ(r->get_header_value("Content-Type"), "application/octet-stream");
  ASSERT_EQ(r->body.size(), 16u);
  EXPECT_EQ(r->body.substr(0, 4), "GMH3");
  // one upload window sets the clock; later measurements go to history
  t.runFor(3 * 3600, 300);
  info = cli->Get("/api/history");
  EXPECT_EQ(info->body.find("\"records\":0"), std::string::npos);
  EXPECT_NE(info->body.find("\"used_bytes\":"), std::string::npos);
  EXPECT_NE(info->body.find("\"max_gap_s\":"), std::string::npos);
  r = cli->Get(all);
  ASSERT_TRUE(r);
  ASSERT_GE(r->body.size(), 16u);
  EXPECT_EQ(r->body.substr(0, 4), "GMH3");
  const size_t rows = static_cast<uint8_t>(r->body[4]) |
                      (static_cast<uint8_t>(r->body[5]) << 8);
  EXPECT_GE(rows, 2u);
  EXPECT_EQ(r->body.size(), 16u + rows * 24u);
  // last row carries device state: battery known, Wi-Fi RSSI after uploads
  const uint8_t* last =
      reinterpret_cast<const uint8_t*>(r->body.data()) + 16 + (rows - 1) * 24u;
  EXPECT_GT(last[20] | (last[21] << 8), 2000);
  EXPECT_LT(static_cast<int8_t>(last[22]), 0);
  EXPECT_EQ(cli->Get("/api/history/24h")->status, 404);
  EXPECT_EQ(cli->Get("/api/history/9/1")->status, 404);
  EXPECT_EQ(cli->Get("/api/history/1/99999999999")->status, 404);
  EXPECT_NE(cli->Get("/api/diag")->body.find("\"used_bytes\":"),
            std::string::npos);
}

TEST_F(PortalTest, BinaryHistoryThroughTunnelMatchesHttpByteForByte) {
  ASSERT_EQ(post("/api/automap", "")->status, 200);
  t.runFor(3 * 3600, 300);
  const char* path = "/api/history/0/4294967295";
  auto http = cli->Get(path);
  ASSERT_TRUE(http);
  ASSERT_EQ(http->status, 200);
  ASSERT_GE(http->body.size(), 16u + 2u * 24u);

  const char* nonce = "0123456789abcdef0123456789abcdef";
  const std::string request =
      gm2test::request("gm-test", nonce, 1, "GET", path, "", "history");
  gm::Portal portal(t.dev->app());
  std::unique_ptr<gm::PortalTunnel> tunnel(
      new gm::PortalTunnel(portal, t.dev->app()));
  tunnel->beginSession(nonce, "gm-test");
  gm::TunnelOut out = {};
  ASSERT_TRUE(tunnel->handle("history", request.data(), request.size(), false,
                             0, &out));
  ASSERT_EQ(out.status, 200);
  EXPECT_EQ(jsoncheck::error(std::string(out.payload, out.len)), "");
  char enc[16];
  ASSERT_TRUE(gm::jsonGetString(out.payload, "enc", enc, sizeof(enc)));
  EXPECT_STREQ(enc, "base64");
  EXPECT_TRUE(gm2test::validMac(out.payload, "gm-test", nonce, 1, "history"));
  EXPECT_EQ(gm2test::plaintext(out.payload, "gm-test", nonce, 1, "history"),
            http->body);
}

TEST_F(PortalTest, RemoteUpdateButtonsNeedPassword) {
  EXPECT_EQ(post("/api/update/check", "", false)->status, 401);
  EXPECT_EQ(post("/api/update/install", "", false)->status, 401);
  ASSERT_EQ(post("/api/config", "update_url=")->status, 200);
  auto r = post("/api/update/check", "");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(r->body.find("\"ok\":false"), std::string::npos) << r->body;
  EXPECT_NE(r->body.find("\"result\":\"failed\""), std::string::npos);
  EXPECT_NE(r->body.find("no update URL"), std::string::npos);
  EXPECT_NE(r->body.find("\"restarting\":false"), std::string::npos);
}

TEST_F(PortalTest, WifiScanListsUniqueVisibleNetworksStrongestFirst) {
  EXPECT_EQ(post("/api/wifi/scan", "", false)->status, 401);
  auto r = post("/api/wifi/scan", "");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200) << r->body;
  const std::string& b = r->body;
  // duplicate collapsed to the strongest entry, hidden network dropped
  EXPECT_EQ(b.find("SimulatedWiFi"), b.rfind("SimulatedWiFi")) << b;
  EXPECT_NE(b.find("\"ssid\":\"SimulatedWiFi\",\"rssi\":-58"),
            std::string::npos);
  EXPECT_EQ(b.find("\"ssid\":\"\""), std::string::npos);
  EXPECT_LT(b.find("SimulatedWiFi"), b.find("Silo-Open"));
  EXPECT_LT(b.find("Silo-Open"), b.find("Neighbour"));
  EXPECT_NE(b.find("\"ssid\":\"Silo-Open\",\"rssi\":-66,\"channel\":11,"
                   "\"secure\":false"),
            std::string::npos);
}

TEST_F(PortalTest, WifiJoinSavesOnlyAfterSuccessfulConnection) {
  EXPECT_EQ(post("/api/wifi/join", "wifi_ssid=Silo-Open", false)->status, 401);
  EXPECT_EQ(post("/api/wifi/join", "wifi_pass=abcdefgh")->status, 400);
  EXPECT_NE(cli->Get("/api/wifi/join")->body.find("\"state\":\"idle\""),
            std::string::npos);

  // wrong password: reported, configuration unchanged
  auto r = post("/api/wifi/join", "wifi_ssid=Neighbour&wifi_pass=wrongpass1");
  ASSERT_EQ(r->status, 200) << r->body;
  EXPECT_NE(cli->Get("/api/wifi/join")->body.find("\"state\":\"pending\""),
            std::string::npos);
  EXPECT_EQ(post("/api/wifi/join", "wifi_ssid=Neighbour")->status, 400);
  t.dev->tick();
  r = cli->Get("/api/wifi/join");
  EXPECT_NE(r->body.find("\"state\":\"failed\""), std::string::npos) << r->body;
  EXPECT_NE(r->body.find("wrong Wi-Fi password"), std::string::npos);
  EXPECT_NE(
      cli->Get("/api/config")->body.find("\"wifi_ssid\":\"SimulatedWiFi\""),
      std::string::npos);

  // success: saved with the IP settings, IP and RSSI reported
  r = post("/api/wifi/join",
           "wifi_ssid=Silo-Open&wifi_pass_clear=1&static_ip=192.168.0.50&"
           "gateway=192.168.0.1&netmask=255.255.255.0&dns=");
  ASSERT_EQ(r->status, 200) << r->body;
  t.dev->tick();
  r = cli->Get("/api/wifi/join");
  EXPECT_NE(r->body.find("\"state\":\"ok\""), std::string::npos) << r->body;
  EXPECT_NE(r->body.find("\"ip\":\"192.168.0.50\""), std::string::npos);
  EXPECT_EQ(r->body.find("wifi_pass"), std::string::npos);
  const std::string cfg = cli->Get("/api/config")->body;
  EXPECT_NE(cfg.find("\"wifi_ssid\":\"Silo-Open\""), std::string::npos) << cfg;
  EXPECT_NE(cfg.find("\"wifi_pass_set\":false"), std::string::npos);
  EXPECT_NE(cfg.find("\"static_ip\":\"192.168.0.50\""), std::string::npos);
}

TEST_F(PortalTest, UdpLogSettingsSurviveRestart) {
  auto r = post("/api/config", "log_udp=1&log_level=2&log_host=&log_port=5514");
  ASSERT_EQ(r->status, 200) << r->body;
  t.dev->restart("test");
  const std::string cfg = cli->Get("/api/config")->body;
  EXPECT_NE(cfg.find("\"log_udp\":true,\"log_dest\":\"udp\","
                     "\"log_level\":2,\"log_host\":\"\","
                     "\"log_port\":5514"),
            std::string::npos)
      << cfg;
  r = post("/api/config", "log_dest=mqtt&log_level=4");
  ASSERT_EQ(r->status, 200) << r->body;
  t.dev->restart("MQTT log settings");
  const std::string mqttCfg = cli->Get("/api/config")->body;
  EXPECT_NE(mqttCfg.find("\"log_udp\":true,\"log_dest\":\"mqtt\","
                         "\"log_level\":4"),
            std::string::npos)
      << mqttCfg;
  EXPECT_EQ(post("/api/config", "log_dest=1")->status, 400);
  EXPECT_EQ(cli->Get("/api/config")->body, mqttCfg);
  EXPECT_EQ(post("/api/crash/clear", "")->status, 400);  // PC: no core dump
}

TEST_F(PortalTest, AlarmImmediateUploadAndNotifications) {
  ASSERT_EQ(post("/api/config",
                 "alarm_temp_c=30&alarm_repeat_min=0&notify_ntfy_url=mock://"
                 "notify/silo")
                ->status,
            200);
  ASSERT_EQ(post("/api/automap", "")->status, 200);
  ASSERT_EQ(post("/api/sim", "action=temp&sensor=1&value=40")->status, 200);
  // Only one sample: the ordinary hourly/four-sample window is not due.
  t.dev->tick();
  auto d = cli->Get("/api/diag");
  ASSERT_TRUE(d);
  EXPECT_NE(d->body.find("\"active\":true"), std::string::npos);
  EXPECT_NE(d->body.find("\"mock_entries_main\":1"), std::string::npos);
  EXPECT_NE(d->body.find("alarm temperatury"), std::string::npos);
  EXPECT_NE(d->body.find("\"topic\":\"silo\""), std::string::npos);
  EXPECT_NE(t.dev->mock().entries(1000001).at(0).values.at("status").find(
                "alarm=temp"),
            std::string::npos);
  // The forced window must leave the ordinary upload schedule untouched.
  EXPECT_FALSE(t.dev->app().state().hasUploadAttempt);
  ASSERT_EQ(post("/api/sim", "action=temp&sensor=1&value=28")->status, 200);
  ASSERT_EQ(post("/api/sim", "action=advance&seconds=900")->status, 200);
  t.dev->tick();
  ASSERT_EQ(post("/api/upload", "")->status, 200);
  d = cli->Get("/api/diag");
  ASSERT_TRUE(d);
  EXPECT_NE(d->body.find("alarm ustąpił"), std::string::npos);
  EXPECT_FALSE(t.dev->app().state().alarm.active);
}

TEST_F(PortalTest, NotifyTestNeedsPasswordAndReportsChannels) {
  auto r = post("/api/notify/test", "", false);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 401);
  ASSERT_EQ(post("/api/config", "notify_ntfy_url=mock://notify/test")->status,
            200);
  r = post("/api/notify/test", "");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(r->body.find("\"ntfy\":{\"enabled\":true,\"ok\":true"),
            std::string::npos);
  EXPECT_NE(r->body.find("\"telegram\":{\"enabled\":false"), std::string::npos);
  auto d = cli->Get("/api/diag");
  ASSERT_TRUE(d);
  EXPECT_NE(d->body.find("test powiadomienia"), std::string::npos);
}

TEST_F(PortalTest, FrostChargingWarning) {
  post("/api/automap", "");
  ASSERT_EQ(post("/api/sim", "action=board_temp&value=-5")->status, 200);
  post("/api/sim", "action=battery&value=3800");
  post("/api/measure", "store=1");
  t.dev->clock().advanceMs(900000);
  post("/api/sim", "action=battery&value=3830");  // +30 mV in 15 min
  post("/api/measure", "store=1");
  std::string s = cli->Get("/api/status")->body;
  EXPECT_NE(s.find("\"charging\":true,\"frost_charging\":true"),
            std::string::npos)
      << s;
  EXPECT_NE(s.find("\"board_temp_c\":-5"), std::string::npos);
  EXPECT_NE(s.find("LiPo must not be charged below 0 C"), std::string::npos);
  EXPECT_GE(t.dev->app().state().alarm.count, 1u);  // queued notification
  // Warmer again: the warning clears.
  post("/api/sim", "action=board_temp&value=10");
  t.dev->clock().advanceMs(900000);
  post("/api/sim", "action=battery&value=3860");
  post("/api/measure", "store=1");
  s = cli->Get("/api/status")->body;
  EXPECT_NE(s.find("\"frost_charging\":false"), std::string::npos) << s;
}

TEST_F(PortalTest, NoChargingGuessWhenDischarging) {
  post("/api/automap", "");
  post("/api/sim", "action=board_temp&value=-5");
  post("/api/sim", "action=battery&value=3800");
  post("/api/measure", "store=1");
  t.dev->clock().advanceMs(900000);
  post("/api/sim", "action=battery&value=3795");
  post("/api/measure", "store=1");
  const std::string s = cli->Get("/api/status")->body;
  EXPECT_NE(s.find("\"charging\":false,\"frost_charging\":false"),
            std::string::npos)
      << s;
}

TEST_F(PortalTest, TalkbackHeartbeatQueuesRemoteRequest) {
  auto r = post("/api/config",
                "talkback_id=7&talkback_key=remote-secret&remote_budget_min=3");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200);
  r = post("/api/sim", "action=talkback&cmd=REMOTE+5+0");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200);
  // No map means a heartbeat upload window (no temperature samples).
  t.dev->tick();
  r = cli->Get("/api/status");
  ASSERT_TRUE(r);
  EXPECT_NE(r->body.find("\"pending_min\":3"), std::string::npos);
  EXPECT_NE(r->body.find("\"talkback_calls\":1"), std::string::npos);
  EXPECT_EQ(r->body.find("remote-secret"), std::string::npos);
  t.dev->tick();
  EXPECT_EQ(t.dev->http().talkbackCount(), 1u);
  uint32_t minutes = 0;
  EXPECT_TRUE(t.dev->app().takeRemoteRequest(&minutes));
  EXPECT_EQ(minutes, 3u);
}

TEST_F(PortalTest, TalkbackDisabledDoesNotConsumeQueue) {
  auto r = post("/api/sim", "action=talkback&cmd=REMOTE+5+0");
  ASSERT_TRUE(r);
  ASSERT_EQ(r->status, 200);
  t.dev->tick();
  EXPECT_EQ(t.dev->http().talkbackCount(), 0u);
  r = cli->Get("/api/status");
  ASSERT_TRUE(r);
  EXPECT_NE(r->body.find("\"pending_min\":0"), std::string::npos);
  r = post("/api/config", "talkback_id=7&talkback_key=remote-secret");
  ASSERT_TRUE(r);
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    t.dev->app().upload();
  }
  r = cli->Get("/api/diag");
  ASSERT_TRUE(r);
  EXPECT_NE(r->body.find("\"pending_min\":5"), std::string::npos);
  EXPECT_EQ(t.dev->http().talkbackCount(), 1u);
}
