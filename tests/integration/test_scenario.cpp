// SPDX-License-Identifier: MIT
// End-to-end PC scenario required before working with the real probe:
// configuration -> reading -> portal -> buffering -> upload -> recovery after
// a network outage, all through the HTTP portal and the simulated device.
#include <gtest/gtest.h>
#include <httplib.h>

#include <set>
#include <string>

#include "platform/web_server.h"
#include "support/test_device.h"

#ifndef GM_TEST_PORTAL_DIR
#define GM_TEST_PORTAL_DIR "portal"
#endif

TEST(Scenario, ConfigureMeasureBufferUploadRecover) {
  TestDevice t("scenario");
  ASSERT_TRUE(t.started);
  gmpc::WebServer web(*t.dev, GM_TEST_PORTAL_DIR);
  const int port = web.start("127.0.0.1", 0);
  ASSERT_GT(port, 0);
  httplib::Client cli("127.0.0.1", port);
  const httplib::Headers auth = {{"X-GM-Password", "test-password"}};
  const char* form = "application/x-www-form-urlencoded";

  // 1. configuration (new channel + key, 15 min / 1 h) and sensor mapping
  auto r = cli.Post("/api/config", auth,
                    "ts_channel=2002002&ts_key=SCENARIOKEY00001&diag_channel="
                    "2002003&diag_key=SCENARIOKEY00002"
                    "&measure_s=900&upload_s=3600",
                    form);
  ASSERT_EQ(r->status, 200) << r->body;
  t.dev->mock().setChannelKey(2002002, "SCENARIOKEY00001");
  ASSERT_EQ(cli.Post("/api/automap", auth, "", form)->status, 200);

  // 2. reading shown in the portal
  r = cli.Post("/api/measure", auth, "", form);
  EXPECT_NE(r->body.find("\"temp_c\":"), std::string::npos);

  // 3. two hours of normal operation
  t.runFor(2 * 3600);
  const size_t afterNormal = t.dev->mock().entries(2002002).size();
  EXPECT_EQ(afterNormal, 8u);

  // 4. outage for 3 hours: samples accumulate, no data loss
  cli.Post("/api/sim", auth, "action=net&arg=down", form);
  t.runFor(3 * 3600);
  EXPECT_EQ(t.dev->mock().entries(2002002).size(), afterNormal);
  const std::string status = cli.Get("/api/status")->body;
  EXPECT_NE(status.find("\"ok\":false"), std::string::npos);
  EXPECT_NE(status.find("simulated outage"), std::string::npos);
  EXPECT_GE(t.dev->app().state().count, 12u);

  // 5. network back: backlog delivered once, buffer drained
  cli.Post("/api/sim", auth, "action=net&arg=up", form);
  t.runFor(2 * 3600);
  const auto entries = t.dev->mock().entries(2002002);
  const gm::RetainedState& st = t.dev->app().state();
  EXPECT_EQ(entries.size() + st.count, static_cast<size_t>(st.nextSeq - 1));
  std::set<std::string> ts;
  for (const auto& e : entries) ts.insert(e.createdAt);
  EXPECT_EQ(ts.size(), entries.size());
  EXPECT_LT(st.count, 4u);
  EXPECT_TRUE(st.last.ok);
  EXPECT_EQ(st.droppedOverflow, 0u);
  // diagnostics channel: first window + once per diag_every_h (6 h) = 2
  EXPECT_EQ(t.dev->mock().entries(2002003).size(), 2u);
  web.stop();
}
