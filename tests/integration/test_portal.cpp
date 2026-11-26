// SPDX-License-Identifier: MIT
// Portal integration: the real PC HTTP server (cpp-httplib) in front of the
// shared portal logic, exercised over TCP with an HTTP client.
#include <gtest/gtest.h>
#include <httplib.h>

#include <memory>
#include <string>

#include "platform/web_server.h"
#include "support/fake_image.h"
#include "support/test_device.h"

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
  auto d = cli->Get("/api/diag");
  ASSERT_TRUE(d);
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
  EXPECT_NE(cfg.find("\"log_udp\":true,\"log_level\":2,\"log_host\":\"\","
                     "\"log_port\":5514"),
            std::string::npos)
      << cfg;
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
