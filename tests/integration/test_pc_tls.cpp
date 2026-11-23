// SPDX-License-Identifier: MIT
// https:// handling of the PC simulator's HTTP client.
#include <gtest/gtest.h>
#include <httplib.h>

#include <cstring>
#include <string>
#include <thread>

#include "sim/mock_thingspeak.h"
#include "sim/sim_network.h"

TEST(PcTls, HttpsMatchesBuildOption) {
  gmpc::MockThingSpeak mock;
  gmpc::PcHttpClient http(mock, ".");
  char resp[64];
  // Plain HTTP server: a TLS handshake against it must fail, it must never
  // fall back to an unencrypted request.
  httplib::Server srv;
  srv.Post("/x", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"success\":true}", "application/json");
  });
  const int port = srv.bind_to_any_port("127.0.0.1");
  ASSERT_GT(port, 0);
  std::thread th([&] { srv.listen_after_bind(); });
  srv.wait_until_ready();
  const std::string url = "https://127.0.0.1:" + std::to_string(port) + "/x";
  const gm::HttpResult r =
      http.postJson(url.c_str(), "{}", 2, 5000, resp, sizeof(resp));
  srv.stop();
  th.join();
  EXPECT_FALSE(r.transportOk);
  if (gmpc::pcTlsEnabled()) {
    EXPECT_EQ(std::strstr(r.error, "GM_PC_TLS"), nullptr) << r.error;
  } else {
    EXPECT_NE(std::strstr(r.error, "GM_PC_TLS=ON"), nullptr) << r.error;
  }
}

TEST(PcTls, PlainHttpStillWorks) {
  gmpc::MockThingSpeak mock;
  gmpc::PcHttpClient http(mock, ".");
  httplib::Server srv;
  srv.Post("/x", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"success\":true}", "application/json");
  });
  const int port = srv.bind_to_any_port("127.0.0.1");
  std::thread th([&] { srv.listen_after_bind(); });
  srv.wait_until_ready();
  char resp[64];
  const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/x";
  const gm::HttpResult r =
      http.postJson(url.c_str(), "{}", 2, 5000, resp, sizeof(resp));
  srv.stop();
  th.join();
  EXPECT_TRUE(r.transportOk) << r.error;
  EXPECT_EQ(r.status, 200);
  EXPECT_STREQ(resp, "{\"success\":true}");
}
