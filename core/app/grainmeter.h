// SPDX-License-Identifier: MIT
#ifndef GM_GRAINMETER_H
#define GM_GRAINMETER_H

#include <stddef.h>
#include <stdint.h>

#include "app/config.h"
#include "app/platform.h"
#include "app/scheduler.h"
#include "data/history_log.h"
#include "data/measurement.h"
#include "data/retained_state.h"
#include "data/rommap.h"
#include "onewire/onewire.h"
#include "ota/ota_session.h"
#include "util/json.h"

#ifndef GM_FW_VERSION
#define GM_FW_VERSION "0.0.0-dev"
#endif

namespace gm {

// Bounded timeouts for one upload window.
enum {
  kWifiConnectTimeoutMs = 20000,
  kTimeSyncTimeoutMs = 10000,
  kHttpTimeoutMs = 15000,
  kMaxUploadBatch = kBufferCapacity,   // << 960 (ThingSpeak free limit)
  kFirmwareDownloadTimeoutMs = 30000,  // per network operation
};

enum class RemoteUpdateResult : uint8_t {
  None = 0,      // never checked
  UpToDate = 1,  // manifest version <= running version
  Updated = 2,   // new image written, restart requested
  Failed = 3,    // manifest/download/verification error (see message)
  Skipped = 4,   // version previously rolled back, or low battery
  ServiceRequested = 5,
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
  const LastUpload& upload();

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

  void writeStatus(JsonWriter& w);  // compact (portal refresh)
  void writeDiag(JsonWriter& w);    // full diagnostics

  const Config& config() const { return cfg_; }
  const RomMap& romMap() const { return map_; }
  RetainedState& state() { return *st_; }
  OtaSession& ota() { return ota_; }
  HistoryLog& history() { return history_; }
  const Ports& ports() const { return p_; }

  // Recent error messages (newest last), shown in portal and diagnostics.
  void recordError(const char* fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 2, 3)))
#endif
      ;

 private:
  void writeMeasure(JsonWriter& w);
  void writeScan(JsonWriter& w);
  void writeBuffer(JsonWriter& w);
  void writeLastUpload(JsonWriter& w);
  void writeErrors(JsonWriter& w);
  void writePower(JsonWriter& w);
  void writeRemoteUpdate(JsonWriter& w);
  bool diagDue(uint32_t nowMono) const;
  bool updateCheckDue(uint32_t nowMono) const;
  void sendDiag(uint32_t nowEpoch);
  void checkRemoteUpdate();
  void setUpdateResult(RemoteUpdateResult r, const char* fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 3, 4)))
#endif
      ;
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
  PowerMode powerMode_;
  OtaSession ota_;
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
