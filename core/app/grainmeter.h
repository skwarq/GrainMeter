// SPDX-License-Identifier: MIT
#ifndef GM_GRAINMETER_H
#define GM_GRAINMETER_H

#include <stddef.h>
#include <stdint.h>

#include "app/battery_forecast.h"
#include "app/config.h"
#include "app/platform.h"
#include "app/scheduler.h"
#include "data/history_log.h"
#include "data/measurement.h"
#include "data/retained_state.h"
#include "data/rommap.h"
#include "onewire/onewire.h"
#include "ota/ota_session.h"
#include "ota/ota_upload.h"
#include "util/json.h"

#ifndef GM_FW_VERSION
#define GM_FW_VERSION "0.0.0-dev"
#endif

namespace gm {

// Bounded timeouts for one upload window.
enum {
  kWifiConnectTimeoutMs = 20000,
  kTimeSyncTimeoutMs = 10000,
  // SNTP only when the clock is unknown or no SNTP/HTTP Date sync for a
  // day: every HTTPS reply from ThingSpeak corrects the drift for free.
  kTimeSyncEveryS = 86400,
  kClockFixMinS = 2,  // smaller Date offsets are rounding, not drift
  // Charging guess: voltage up by >= 8 mV and >= 20 mV/h between measurements
  // (the charger raises the cell voltage; ADC noise is ~5 mV after averaging).
  kChargeRiseMinMv = 8,
  kChargeRiseMvPerH = 20,
  // Frost warning on/off at the board temperature (hysteresis); the chip runs
  // a little warmer than the cell, hence the margin above 0 °C.
  kFrostOnCenti = 200,
  kFrostOffCenti = 400,
  kHttpTimeoutMs = 15000,
  kMaxUploadBatch = kBufferCapacity,   // << 960 (ThingSpeak free limit)
  kFirmwareDownloadTimeoutMs = 30000,  // per network operation
  // Whole download; with Wi-Fi, time sync and manifest it stays below the
  // portal's 240 s wait for the install button.
  kFirmwareDownloadTotalS = 150,
};

enum class RemoteUpdateResult : uint8_t {
  None = 0,      // never checked
  UpToDate = 1,  // manifest version <= running version
  Updated = 2,   // new image written, restart requested
  Failed = 3,    // manifest/download/verification error (see message)
  Skipped = 4,   // version previously rolled back, or low battery
  ServiceRequested = 5,
  Available = 6,  // newer version found by a check without installation
};
const char* remoteUpdateResultName(RemoteUpdateResult r);

struct Ports {
  OneWireBus* bus;
  Clock* clock;
  KeyValueStore* store;
  Network* net;
  HttpClient* http;
  SystemPort* sys;
  OtaTarget* ota;
  HistoryStorage* history = nullptr;  // optional local history
  RemoteSessionPort* remote = nullptr;
};

// Application logic shared by the ESP32 firmware and the PC simulator.
// Not thread-safe: platforms serialize access (one mutex on ESP32).
class GrainMeter {
 public:
  GrainMeter(const Ports& ports, RetainedState* state,
             const ConfigLimits& limits, const char* defaultReportUrl,
             const char* defaultUpdateUrl = "");

  // Loads configuration and validates the retained state (re-initialised
  // when corrupt / after power loss). Returns false if the configuration
  // store failed (device still runs with defaults).
  bool begin();

  const ScanReport& scan();
  // Reads all sensors; with store=true a Sample is appended to the buffer
  // and the measurement schedule advances.
  const MeasureReport& measure(bool store);
  // One upload window: Wi-Fi up, time, bulk write, diagnostic write, Wi-Fi
  // down. Buffer entries are removed only after a confirmed write.
  const LastUpload& upload(bool alarmWindow = false);
  // Station must already be connected; bounded SNTP, no radio lifecycle.
  // Also used before starting a remote session with an unknown clock.
  bool synchronizeTime();
  void notifyTest(JsonWriter& w);

  // Runs whatever the schedule says is due. allowUpload=false keeps the
  // radio untouched (service mode). Returns seconds until next measurement.
  uint32_t runDue(bool allowUpload);
  PowerMode powerMode() const { return powerMode_; }

  // Configuration and mapping changes (persisted immediately).
  bool applyConfig(const Form& form, char* err, size_t cap);
  bool mapAssign(const char* romHex, long point, long heightCm, char* err,
                 size_t cap);
  bool mapRemove(const char* romHex, char* err, size_t cap);
  size_t mapAuto();

  bool checkServicePassword(const char* pass) const;

  // Wi-Fi join from the portal (network picked from a scan): the request
  // only validates and queues; the platform loop runs it with
  // wifiJoinTake() -> Network::connect() -> wifiJoinFinish(), outside the
  // device lock on ESP32 (connecting takes up to 20 s while the portal
  // polls GET /api/wifi/join). On success the Wi-Fi and IP fields are saved.
  // Form keys: wifi_ssid, wifi_pass, wifi_pass_clear, static_ip, gateway,
  // netmask, dns (empty password = keep the stored one).
  bool wifiJoinRequest(const Form& form, char* err, size_t cap);
  bool wifiJoinTake(Config* candidate);  // pending -> running
  void wifiJoinFinish(const Config& candidate, bool ok, const char* msg);
  // Convenience for platforms that may block under their lock (PC).
  void runWifiJoin();
  void writeWifiJoin(JsonWriter& w);

  void writeStatus(JsonWriter& w);  // compact (portal refresh)
  void writeDiag(JsonWriter& w);    // full diagnostics

  const Config& config() const { return cfg_; }
  const RomMap& romMap() const { return map_; }
  RetainedState& state() { return *st_; }
  OtaSession& ota() { return ota_; }
  OtaUpload& otaUpload() { return otaUpload_; }
  HistoryLog& history() { return history_; }
  // Portal action: connects Wi-Fi now, reads the update manifest and, with
  // install, downloads/verifies/installs a newer version (restart requested
  // on success). Works also with periodic checks off (update URL needed).
  RemoteUpdateResult remoteUpdateNow(bool install, const char* expect = "");
  // Message for the portal reply: a refusal before any network access
  // (not stored, the last real check result stays) or the check result.
  const char* remoteUpdateMessage() const {
    return manualMsg_[0] != '\0' ? manualMsg_ : st_->updateMessage;
  }
  // Wall-clock time of the last measurement (0 = unknown).
  uint32_t lastMeasureEpoch() const;
  // Rows of the local history further apart than this = no data between.
  uint32_t historyMaxGapS() const;
  // {available, records, used_bytes, capacity_bytes, ...} (reads the log).
  void writeHistoryInfo(JsonWriter& w);
  // Consumes one pending request; caller accounts actual battery-powered
  // session time with remoteSessionUsed (USB service time is exempt).
  bool takeRemoteRequest(uint32_t* minutes);
  void remoteSessionUsed(uint32_t seconds);
  RemoteUpdateResult lastRemoteUpdateResult() const {
    return static_cast<RemoteUpdateResult>(st_->updateResult);
  }
  const char* lastRemoteUpdateVersion() const { return st_->updateLatest; }
  uint32_t remoteBudgetLeftS() { return remoteLeftS(); }
  const Ports& ports() const { return p_; }

  // Recent error messages (newest last), shown in portal and diagnostics.
  void recordError(const char* fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 2, 3)))
#endif
      ;

 private:
  size_t assignPointHints(RomMap& next, const PointHint* hints, size_t n,
                          uint8_t conflictPoints[kMaxPoints],
                          size_t* conflicts);
  bool saveHintMap(const RomMap& next, const PointHint* hints, size_t n);
  void pollTalkback();
  void refreshRemoteBudget();
  uint32_t remoteLeftS();
  uint32_t remotePendingMin();
  void writeRemote(JsonWriter& w);
  uint32_t pendingRemoteMin_ = 0;
  uint32_t pendingRemoteUntil_ = 0;
  char lastRemoteCmd_[128] = "";
  uint32_t lastRemoteEpoch_ = 0;
  uint32_t lastRemoteMonoS_ = 0;
  void writeAlarm(JsonWriter& w);
  void sendNotifications();
  void writeMeasure(JsonWriter& w);
  void writeScan(JsonWriter& w);
  void writeBuffer(JsonWriter& w);
  void writeLastUpload(JsonWriter& w);
  void writeErrors(JsonWriter& w);
  void writePower(JsonWriter& w);
  void writeRemoteUpdate(JsonWriter& w);
  bool diagDue(uint32_t nowMono) const;
  bool updateCheckDue(uint32_t nowMono) const;
  // Battery points in bulk, plus the full diagnostic row when `full`.
  void sendDiag(uint32_t nowEpoch, uint32_t nowMono, bool full);
  bool timeSyncDue(uint32_t nowMono) const;
  // Updates the charging guess and raises the frost-charging warning.
  void checkCharging(int mv, uint32_t monoS);
  void noteTimeSync(uint8_t source);
  // Corrects the wall clock from a verified HTTPS reply (Date header).
  void adjustClock(const HttpResult& r);
  void writeClock(JsonWriter& w);
  // install = false: only reads the manifest and reports what is available.
  // manual (portal buttons): does not touch the periodic schedule and does
  // not consume a remote service request (the technician is already here);
  // expect = version the user confirmed ("" = any newer).
  void checkRemoteUpdate(bool install, bool manual, const char* expect);
  void setUpdateResult(RemoteUpdateResult r, const char* fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 3, 4)))
#endif
      ;
  HistoryRecord historyRecord(const Sample& s, bool reconstructed);
  void stampSampleTimes(uint32_t nowEpoch, uint32_t nowMono);
  void finishUpload(bool ok, int status, const char* msg);

  Ports p_;
  RetainedState* st_;
  ConfigLimits limits_;
  const char* defaultReportUrl_;
  const char* defaultUpdateUrl_;
  Config cfg_;
  RomMap map_;
  ScanReport scan_;
  MeasureReport meas_;
  bool haveScan_;
  bool haveMeasure_;
  bool configOk_;
  bool stateRecovered_;
  // Last manual check from the portal (display only; RAM is enough, the
  // device stays awake in service mode). The periodic schedule uses
  // st_->lastUpdateCheckMonoS.
  bool manualCheck_ = false;
  uint32_t manualCheckMonoS_ = 0;
  bool manualRun_ = false;  // failures of a button press are not device errors
  char manualMsg_[96] = "";
  RemoteUpdateResult refuseManual(RemoteUpdateResult r, const char* msg);
  enum class JoinState : uint8_t { Idle, Pending, Running, Ok, Failed };
  JoinState joinState_ = JoinState::Idle;
  Config joinCfg_;  // candidate (holds a password: never serialised)
  char joinSsid_[33] = "";
  char joinMsg_[96] = "";
  char joinIp_[16] = "";
  int joinRssi_ = 0;
  // Battery forecast from the history, reused for 10 min (status polling).
  BatteryForecast forecast_;
  uint32_t forecastAt_ = 0;
  PowerMode powerMode_;
  OtaSession ota_;
  OtaUpload otaUpload_;
  HistoryLog history_;

  enum { kErrorSlots = 8 };
  struct ErrorEntry {
    uint32_t monoS;
    char text[80];
  };
  ErrorEntry errors_[kErrorSlots];
  size_t errorCount_;
  size_t errorNext_;

  char bodyBuf_[24576];  // fits a full 96-sample bulk request
  char respBuf_[512];
};

}  // namespace gm

#endif
