// SPDX-License-Identifier: MIT
#ifndef GM_PC_SIM_NETWORK_H
#define GM_PC_SIM_NETWORK_H

#include <atomic>
#include <deque>
#include <memory>
#include <string>

#include "app/platform.h"
#include "sim/mock_thingspeak.h"
#include "sim/sim_clock.h"

namespace gmpc {

// Simulated Wi-Fi station. "Outage" makes connect() fail like a missing AP;
// a successful connect also provides simulated SNTP time.
class SimNetwork : public gm::Network {
 public:
  explicit SimNetwork(SimClock& clock)
      : clock_(clock), up_(true), connected_(false), connects_(0), syncs_(0) {}

  bool connect(const gm::Config& cfg, uint32_t timeoutMs, char* err,
               size_t errCap) override;
  void disconnect() override { connected_ = false; }
  bool syncTime(uint32_t timeoutMs) override;
  int rssi() override { return connected_ ? -58 : 0; }
  const char* stateText() override;
  const char* ipText() override { return connected_ ? "192.168.0.50" : ""; }
  // Fixed list of simulated access points (with a duplicate and a hidden
  // one). Joining "Neighbour" fails like a wrong password; every other SSID
  // connects.
  int scan(gm::WifiNetwork* out, int max) override;

  void setUp(bool up) { up_ = up; }
  bool up() const { return up_; }
  unsigned connectCount() const { return connects_; }
  unsigned syncCount() const { return syncs_; }

 private:
  SimClock& clock_;
  std::atomic<bool> up_;
  std::atomic<bool> connected_;
  std::atomic<unsigned> connects_;
  std::atomic<unsigned> syncs_;
};

// HTTP client for the simulator: mock://thingspeak goes to the in-process
// MockThingSpeak, mock://updates/<file> serves files from <dataDir>/updates
// (remote firmware update tests), http:// reaches a real local server
// (e.g. gm_receiver) via cpp-httplib. https:// (real ThingSpeak, GitHub
// releases) only in a GM_PC_TLS build, with certificate verification.
class PcHttpClient : public gm::HttpClient {
 public:
  PcHttpClient(MockThingSpeak& mock, const std::string& updatesDir)
      : mock_(mock),
        updatesDir_(updatesDir),
        lostResponse_(false),
        posts_(0),
        sessions_(0) {}

  gm::HttpResult postJson(const char* url, const char* body, size_t len,
                          uint32_t timeoutMs, char* resp,
                          size_t respCap) override;
  gm::HttpResult get(const char* url, uint32_t timeoutMs, char* resp,
                     size_t respCap) override;
  gm::HttpResult download(const char* url, uint32_t timeoutMs,
                          gm::DownloadSink& sink) override;
  void endSession() override { ++sessions_; }

  // Server stores the data but the reply never arrives (tests idempotence).
  void setLostResponse(bool on) { lostResponse_ = on; }
  bool lostResponse() const { return lostResponse_; }
  void queueTalkback(const char* command) {
    talkbackCommands_.push_back(command);
  }
  unsigned talkbackCount() const { return talkbackCalls_; }
  unsigned postCount() const { return posts_; }
  unsigned sessionCount() const { return sessions_; }
  std::string lastUrl();
  void writeNotifyJson(gm::JsonWriter& w);
  // Mock replies carry the simulator's true time as HTTP Date.
  void setClock(SimClock* clock) { clock_ = clock; }

 private:
  bool readUpdateFile(const char* url, std::string* out, gm::HttpResult* res);

  MockThingSpeak& mock_;
  std::string updatesDir_;
  std::atomic<bool> lostResponse_;
  std::atomic<unsigned> posts_;
  std::atomic<unsigned> sessions_;
  std::string lastUrl_;
  std::deque<std::string> notifications_;
  std::deque<std::string> talkbackCommands_;
  unsigned talkbackCalls_ = 0;
  SimClock* clock_ = nullptr;
};

// True when built with GM_PC_TLS (https:// to real servers allowed).
bool pcTlsEnabled();

}  // namespace gmpc

#endif
