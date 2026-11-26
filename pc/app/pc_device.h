// SPDX-License-Identifier: MIT
#ifndef GM_PC_DEVICE_H
#define GM_PC_DEVICE_H

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "app/grainmeter.h"
#include "app/portal.h"
#include "app/portal_tunnel.h"
#include "app/shell.h"
#include "platform/file_store.h"
#include "platform/history_file.h"
#include "sim/mock_thingspeak.h"
#include "sim/sim_clock.h"
#include "sim/sim_network.h"
#include "sim/sim_ota.h"
#include "sim/sim_probe.h"
#include "util/log_buffer.h"

namespace gmpc {

// The simulated GrainMeter: shared application logic + simulated hardware.
// All entry points lock one mutex (HTTP threads, console, scheduler loop).
class PcDevice : public gm::SystemPort,
                 public gm::RemoteSessionPort,
                 public gm::LogSource {
 public:
  struct Options {
    std::string dataDir = "pc-data";
    std::string configPath;    // default <dataDir>/grainmeter-pc.conf
    double speed = 60.0;       // simulated seconds per real second
    bool manualClock = false;  // tests: time moves only when advanced
    // Start with the PC's real wall clock (history works at once). false =
    // like a fresh ESP32: no time until the first upload window (SNTP).
    bool wallClockAtStart = true;
    int sensors = 8;
    std::string servicePassword;    // empty: keep stored / generate
    bool provisionDefaults = true;  // fill simulated Wi-Fi + channels
  };

  explicit PcDevice(const Options& opt);

  bool start(std::string* error);
  // Runs due work (measurement / upload). Returns simulated seconds until
  // the next measurement.
  uint32_t tick();

  bool handle(const gm::PortalRequest& req, gm::PortalReply* reply,
              std::string* bodyCopy);
  bool otaStart(const char* password, size_t len, gm::OtaBody kind,
                const char* name, gm::PortalReply* reply, std::string* body);
  bool otaFeed(const uint8_t* data, size_t len);
  void otaAbort(const char* reason);
  void otaFinish(gm::PortalReply* reply, std::string* body);
  // In-process MQTT stand-in: signed tunnel requests and retained status.
  void beginRemoteSession(const char* nonce, const char* id);
  bool handleTunnel(const char* mid, const std::string& payload, bool retained,
                    gm::TunnelOut* out, std::string* bodyCopy);
  std::string remoteStatus();
  std::string shell(const std::string& line, bool* restart);

  // Simulated restart (after OTA or on request): RTC memory survives.
  void restart(const char* reason);
  bool restartPending() const { return restartPending_; }

  // gm::SystemPort
  gm::LogSource* logSource() override { return this; }
  bool read(uint64_t since, char* out, size_t cap) override;
  const char* platformName() override { return "pc-sim"; }
  int batteryMv() override;
  int32_t batteryMahPerDayCenti() override { return batteryMahPerDayCenti_; }
  const char* wakeReason() override { return wakeReason_.c_str(); }
  const char* resetReason() override { return resetReason_.c_str(); }
  const char* rejectedFirmware() override {
    return ota_.rejectedVersion().c_str();
  }
  void requestRestart() override { restartPending_ = true; }
  bool boardTempCenti(int16_t* out) override {
    *out = static_cast<int16_t>(boardTempCenti_.load());
    return true;
  }
  void writePlatformJson(gm::JsonWriter& w, bool detailed) override;
  bool platformAction(const char* action, const gm::Form& args, char* msg,
                      size_t msgCap) override;

  bool active() override { return remoteActive_; }
  gm::RemoteSessionInfo info() override;
  gm::RemoteExtendResult extend(uint32_t minutes,
                                gm::RemoteSessionInfo* out) override;
  void validRequest() override;

  // Access for tests.
  void setBatteryMahPerDayCenti(int32_t value) {
    batteryMahPerDayCenti_ = value;
  }
  SimClock& clock() { return clock_; }
  SimProbe& probe() { return probe_; }
  SimNetwork& network() { return net_; }
  PcHttpClient& http() { return http_; }
  MockThingSpeak& mock() { return mock_; }
  SimOtaTarget& ota() { return ota_; }
  gm::GrainMeter& app() { return *app_; }
  std::mutex& mutex() { return mu_; }
  const std::string& configPath() const { return store_.path(); }

 private:
  bool simAction(const gm::Form& args, char* msg, size_t cap);
  void provision();
  void pollRemote();
  void accountRemote();

  Options opt_;
  std::unique_ptr<gm::LogBuffer> logs_;
  uint64_t logLast_ = 0;
  std::chrono::steady_clock::time_point logReadAt_;
  std::mutex mu_;
  SimClock clock_;
  SimProbe probe_;
  SimNetwork net_;
  MockThingSpeak mock_;
  PcHttpClient http_;
  SimOtaTarget ota_;
  FileStore store_;
  FileHistoryStorage history_;  // "flash" history, survives restarts
  gm::RetainedState retained_;  // "RTC memory"
  std::unique_ptr<gm::GrainMeter> app_;
  std::unique_ptr<gm::Portal> portal_;
  std::unique_ptr<gm::PortalTunnel> tunnel_;
  std::string remoteNonce_, remoteId_, remoteOpMid_, remoteOpTarget_;
  std::string runningVersion_ = GM_FW_VERSION;
  bool remoteOpOk_ = false;
  bool remoteOpReported_ = false;
  std::atomic<bool> restartPending_;
  std::string wakeReason_;
  std::string resetReason_;
  std::atomic<int> batteryOverrideMv_{0};
  std::atomic<int32_t> batteryMahPerDayCenti_{-1};
  std::atomic<int> boardTempCenti_{2000};  // sim board_temp action
  std::atomic<unsigned> serviceRequests_{0};
  bool remoteActive_ = false;
  uint32_t remoteUntil_ = 0, remoteLastValid_ = 0, remoteAccounted_ = 0;
};

}  // namespace gmpc

#endif
