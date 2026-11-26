// SPDX-License-Identifier: MIT
// Remote firmware update, battery protection and the upload-window budget,
// exercised through the shared logic with the simulated device.
#include <gtest/gtest.h>
#include <httplib.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "support/fake_image.h"
#include "support/test_device.h"
#include "util/sha256.h"

namespace {

std::string hexSha(const std::vector<uint8_t>& data) {
  gm::Sha256 h;
  h.update(data.data(), data.size());
  uint8_t d[32];
  h.finish(d);
  char hex[65];
  gm::sha256ToHex(d, hex);
  return hex;
}

class RemoteUpdateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(t.started);
    std::filesystem::create_directories(t.dir + "/updates");
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    ASSERT_EQ(t.dev->app().mapAuto(), 8u);
    configure(
        "update_url=mock%3A%2F%2Fupdates%2Fmanifest.json&update_check_h=24");
  }
  void configure(const char* body) {
    char err[96];
    ASSERT_TRUE(t.dev->app().applyConfig(gm::Form(body, std::strlen(body)), err,
                                         sizeof(err)))
        << err;
  }
  void publish(const std::string& version, const std::vector<uint8_t>& img,
               const std::string& sha, const std::string& extra = "") {
    std::ofstream(t.dir + "/updates/fw.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(img.data()),
               static_cast<std::streamsize>(img.size()));
    std::ofstream(t.dir + "/updates/manifest.json")
        << "{\"version\":\"" << version
        << "\",\"url\":\"mock://updates/fw.bin\",\"sha256\":\"" << sha << "\""
        << extra << "}";
  }
  std::string result() { return t.dev->app().state().updateMessage; }
  gm::RemoteUpdateResult code() {
    return static_cast<gm::RemoteUpdateResult>(
        t.dev->app().state().updateResult);
  }
  void sim(const char* body) {
    char msg[128];
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    ASSERT_TRUE(t.dev->platformAction("sim", gm::Form(body, std::strlen(body)),
                                      msg, sizeof(msg)))
        << msg;
  }

  TestDevice t{"remote"};
};

}  // namespace

TEST_F(RemoteUpdateTest, ScheduledCheckOnlyInformsThenInstallOnRequest) {
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  publish("9.9.9", img, hexSha(img));
  t.runFor(3600);  // first upload window checks the manifest
  // A scheduled check never installs by itself: it only announces.
  EXPECT_EQ(code(), gm::RemoteUpdateResult::Available) << result();
  EXPECT_STREQ(t.dev->app().state().updateLatest, "9.9.9");
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
  t.runFor(25 * 3600);  // still only announced a day later
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    EXPECT_EQ(t.dev->app().remoteUpdateNow(true),
              gm::RemoteUpdateResult::Updated);
  }
  t.runFor(60);
  EXPECT_FALSE(t.dev->restartPending());  // tick() already restarted
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_1");
  // samples measured before the update survive the (software) restart
  EXPECT_TRUE(t.dev->app().state().valid());
}

TEST_F(RemoteUpdateTest, ManualCheckThenInstallFromThePortal) {
  configure("update_check_h=0");  // periodic checks off: buttons still work
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  publish("9.9.9", img, hexSha(img));
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    EXPECT_EQ(t.dev->app().remoteUpdateNow(false),
              gm::RemoteUpdateResult::Available);
  }
  EXPECT_NE(result().find("9.9.9 available"), std::string::npos) << result();
  EXPECT_STREQ(t.dev->app().state().updateLatest, "9.9.9");
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");  // nothing downloaded
  EXPECT_FALSE(t.dev->restartPending());
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    EXPECT_EQ(t.dev->app().remoteUpdateNow(true),
              gm::RemoteUpdateResult::Updated);
  }
  EXPECT_TRUE(t.dev->restartPending());
  EXPECT_EQ(t.dev->network().rssi(), 0);  // disconnected again
}

TEST_F(RemoteUpdateTest, ManualButtonsKeepScheduleAndServiceRequest) {
  configure("update_check_h=24");
  std::ofstream(t.dir + "/updates/manifest.json")
      << "{\"service_id\":5,\"service_min\":15}";
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    t.dev->app().remoteUpdateNow(false);
    EXPECT_FALSE(t.dev->app().state().hasUpdateCheck);  // schedule untouched
    char st[16384];
    gm::JsonWriter sw(st, sizeof(st));
    t.dev->app().writeStatus(sw);
    // ...but the portal shows when it was checked
    EXPECT_NE(std::string(st).find("\"last_check_age_s\":0"), std::string::npos)
        << st;
    char buf[8192];
    gm::JsonWriter w(buf, sizeof(buf));
    t.dev->writePlatformJson(w, true);
    // the technician is already on site: the request stays for later
    EXPECT_NE(std::string(buf).find("\"service_requests\":0"),
              std::string::npos)
        << buf;
  }
  EXPECT_EQ(code(), gm::RemoteUpdateResult::UpToDate) << result();
}

TEST_F(RemoteUpdateTest, InstallOnlyTheConfirmedVersion) {
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  publish("9.9.9", img, hexSha(img));
  std::lock_guard<std::mutex> lock(t.dev->mutex());
  EXPECT_EQ(t.dev->app().remoteUpdateNow(true, "9.9.8"),
            gm::RemoteUpdateResult::Available);
  EXPECT_NE(result().find("check again"), std::string::npos) << result();
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
  EXPECT_FALSE(t.dev->restartPending());
  EXPECT_EQ(t.dev->app().remoteUpdateNow(true, "9.9.9"),
            gm::RemoteUpdateResult::Updated);
}

TEST_F(RemoteUpdateTest, ManualCheckRefusedOnCriticalBattery) {
  sim("action=battery&value=3200");
  const unsigned connects = t.dev->network().connectCount();
  std::lock_guard<std::mutex> lock(t.dev->mutex());
  EXPECT_EQ(t.dev->app().remoteUpdateNow(false),
            gm::RemoteUpdateResult::Skipped);
  EXPECT_EQ(t.dev->network().connectCount(), connects);
}

TEST_F(RemoteUpdateTest, StaleAvailableClearedAfterVersionChange) {
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  publish("9.9.9", img, hexSha(img));
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    ASSERT_EQ(t.dev->app().remoteUpdateNow(false),
              gm::RemoteUpdateResult::Available);
    // as if the running firmware were already 9.9.9 (installed from a file)
    std::strcpy(t.dev->app().state().updateLatest, GM_FW_VERSION);
    t.dev->app().state().seal();
  }
  t.dev->requestRestart();
  t.runFor(60);
  EXPECT_EQ(code(), gm::RemoteUpdateResult::UpToDate) << result();
}

TEST_F(RemoteUpdateTest, ManualCheckNeedsUpdateUrl) {
  configure("update_url=");
  std::lock_guard<std::mutex> lock(t.dev->mutex());
  EXPECT_EQ(t.dev->app().remoteUpdateNow(false),
            gm::RemoteUpdateResult::Failed);
  EXPECT_NE(
      std::string(t.dev->app().remoteUpdateMessage()).find("no update URL"),
      std::string::npos);
  // a refused button press is not stored and is not a device error
  EXPECT_EQ(code(), gm::RemoteUpdateResult::None);
  char buf[16384];
  gm::JsonWriter w(buf, sizeof(buf));
  t.dev->app().writeDiag(w);
  EXPECT_EQ(std::string(buf).find("remote update"), std::string::npos) << buf;
}

TEST_F(RemoteUpdateTest, SameVersionIsNotDownloaded) {
  const auto img = fakeImage(150000, 0x000D, "grainmeter", GM_FW_VERSION);
  publish(GM_FW_VERSION, img, hexSha(img));
  t.runFor(3600);
  EXPECT_EQ(code(), gm::RemoteUpdateResult::UpToDate) << result();
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
}

TEST_F(RemoteUpdateTest, WrongHashLeavesBootPartitionUnchanged) {
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  publish("9.9.9", img, std::string(64, '0'));
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    t.dev->app().remoteUpdateNow(true);  // the user's "Install" click
  }
  t.runFor(60);
  EXPECT_EQ(code(), gm::RemoteUpdateResult::Failed);
  EXPECT_NE(result().find("sha256"), std::string::npos);
  EXPECT_FALSE(t.dev->ota().pending());
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
}

TEST_F(RemoteUpdateTest, ForeignImageRejectedEvenWithMatchingHash) {
  const auto img = fakeImage(150000, 0x0002, "grainmeter", "9.9.9");  // S2
  publish("9.9.9", img, hexSha(img));
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    t.dev->app().remoteUpdateNow(true);  // the user's "Install" click
  }
  t.runFor(60);
  EXPECT_EQ(code(), gm::RemoteUpdateResult::Failed);
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
}

TEST_F(RemoteUpdateTest, RolledBackVersionIsNotRetried) {
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  publish("9.9.9", img, hexSha(img));
  sim("action=ota_fail_boot&arg=on");
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    EXPECT_EQ(t.dev->app().remoteUpdateNow(true),
              gm::RemoteUpdateResult::Updated);
  }
  t.runFor(3600);
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");  // rolled back
  EXPECT_EQ(t.dev->ota().rejectedVersion(), "9.9.9");
  t.runFor(25 * 3600);  // next daily check
  EXPECT_EQ(code(), gm::RemoteUpdateResult::Skipped) << result();
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
}

TEST_F(RemoteUpdateTest, ServiceWindowRequestedOncePerId) {
  std::ofstream(t.dir + "/updates/manifest.json")
      << "{\"service_id\":5,\"service_min\":15}";
  t.runFor(3600);
  t.runFor(25 * 3600);
  std::lock_guard<std::mutex> lock(t.dev->mutex());
  char buf[8192];
  gm::JsonWriter w(buf, sizeof(buf));
  t.dev->writePlatformJson(w, true);
  EXPECT_NE(std::string(buf).find("\"service_requests\":1"), std::string::npos)
      << buf;
}

TEST_F(RemoteUpdateTest, LowBatterySkipsUpdateAndCriticalStopsRadio) {
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  publish("9.9.9", img, hexSha(img));
  sim("action=battery&value=3600");  // economy (< 3650)
  t.runFor(4 * 3600 + 1800);
  // Scheduled checks only announce, also on a low battery.
  EXPECT_EQ(code(), gm::RemoteUpdateResult::Available) << result();
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_0");
  const unsigned connects = t.dev->network().connectCount();
  EXPECT_LE(connects, 2u);  // every 4 h instead of hourly

  sim("action=battery&value=3200");  // critical
  t.runFor(8 * 3600);
  EXPECT_EQ(t.dev->network().connectCount(), connects);  // radio stays off
  EXPECT_GT(t.dev->app().state().count, 4u);             // still measuring
}

TEST_F(RemoteUpdateTest, OneSessionPerWindowDateKeepsTheClock) {
  configure("update_check_h=0");
  t.runFor(3 * 3600);
  const unsigned windows = t.dev->network().connectCount();
  EXPECT_EQ(windows, 3u);
  // SNTP once; afterwards the HTTP Date of the ThingSpeak replies keeps the
  // drifting RC clock right.
  EXPECT_EQ(t.dev->network().syncCount(), 1u);
  EXPECT_EQ(t.dev->http().sessionCount(), windows);
}

// GitHub-style release URLs: /releases/latest/download/<file> answers 302 to
// the concrete release, whose assets redirect again to a storage host.
TEST_F(RemoteUpdateTest, FollowsReleaseRedirects) {
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  const std::string bin(img.begin(), img.end());
  httplib::Server srv;
  int port = 0;
  std::string manifest;
  srv.Get("/releases/latest/download/manifest.json",
          [](const httplib::Request&, httplib::Response& res) {
            res.set_redirect("/releases/download/v9.9.9/manifest.json");
          });
  srv.Get("/releases/download/v9.9.9/manifest.json",
          [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(manifest, "application/json");
          });
  srv.Get("/releases/download/v9.9.9/fw.bin",
          [](const httplib::Request&, httplib::Response& res) {
            res.set_redirect("/storage/abc123");
          });
  srv.Get("/storage/abc123",
          [&](const httplib::Request&, httplib::Response& res) {
            res.set_content(bin, "application/octet-stream");
          });
  port = srv.bind_to_any_port("127.0.0.1");
  ASSERT_GT(port, 0);
  std::thread th([&] { srv.listen_after_bind(); });
  srv.wait_until_ready();
  const std::string base = "http://127.0.0.1:" + std::to_string(port);
  manifest = "{\"version\":\"9.9.9\",\"url\":\"" + base +
             "/releases/download/v9.9.9/fw.bin\",\"sha256\":\"" + hexSha(img) +
             "\",\"size\":150000}";
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    const std::string body = "update_url=" + base +
                             "%2Freleases%2Flatest%2Fdownload%2Fmanifest.json";
    configure(body.c_str());
  }
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    t.dev->app().remoteUpdateNow(true);  // the user's "Install" click
  }
  t.runFor(60);
  srv.stop();
  th.join();
  EXPECT_EQ(code(), gm::RemoteUpdateResult::Updated) << result();
  EXPECT_STREQ(t.dev->ota().runningSlot(), "ota_1");
}

TEST_F(RemoteUpdateTest, QueuedRemoteSessionDefersTheManifestCheck) {
  // A waiting user (TalkBack REMOTE) must not wait for a manifest download:
  // the check moves to the next window, the session request is kept.
  configure("talkback_id=7&talkback_key=remote-secret&remote_budget_min=30");
  const auto img = fakeImage(150000, 0x000D, "grainmeter", "9.9.9");
  publish("9.9.9", img, hexSha(img));
  sim("action=talkback&cmd=REMOTE+5+0");
  t.runFor(3600);  // first upload window: TalkBack has a command
  EXPECT_STREQ(t.dev->app().state().updateLatest, "");  // not checked
  uint32_t minutes = 0;
  {
    std::lock_guard<std::mutex> lock(t.dev->mutex());
    EXPECT_TRUE(t.dev->app().takeRemoteRequest(&minutes));
  }
  EXPECT_EQ(minutes, 5u);
  t.runFor(3600);  // next window, nothing pending: the check runs
  EXPECT_STREQ(t.dev->app().state().updateLatest, "9.9.9");
}
