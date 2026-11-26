// SPDX-License-Identifier: MIT
#ifndef GM_PLATFORM_H
#define GM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "app/config.h"
#include "util/form.h"
#include "util/json.h"

namespace gm {

struct RemoteSessionInfo {
  uint32_t remainingS = 0;
  uint32_t budgetLeftS = 0;
};

enum class RemoteExtendResult { Ok, NoSession, Budget };

// Calls are serialized with the application lock, including budget accounting.
class RemoteSessionPort {
 public:
  virtual ~RemoteSessionPort() {}
  virtual bool active() = 0;
  virtual RemoteSessionInfo info() = 0;
  virtual void validRequest() = 0;
  virtual RemoteExtendResult extend(uint32_t minutes,
                                    RemoteSessionInfo* out) = 0;
};

inline uint32_t remoteExtendedRemaining(uint32_t remaining, uint32_t minutes,
                                        uint32_t budgetLeft) {
  uint32_t seconds = remaining + minutes * 60;
  if (seconds > 3600) seconds = 3600;
  return seconds > budgetLeft ? budgetLeft : seconds;
}

// System/hardware boundary. Everything below is implemented twice:
// firmware/main (ESP-IDF) and pc/ (simulation). Shared logic only talks to
// these interfaces.

class Clock {
 public:
  virtual ~Clock() {}
  // Monotonic seconds since power-on; continues across deep sleep (RTC
  // timer on ESP32, simulated/accelerated on PC).
  virtual uint32_t monoS() = 0;
  // UTC wall clock, valid after synchronization or checked RTC recovery.
  virtual bool wallValid() = 0;
  virtual uint32_t epoch() = 0;
  // True for a checked recovery on this boot, not a network synchronization.
  virtual bool wallRestored() { return false; }
  // Refresh a platform's independent crash/restart snapshot, if supported.
  virtual void checkpoint() {}
  // Sets the wall clock (HTTP Date correction). false = not supported.
  virtual bool setEpoch(uint32_t epoch) {
    (void)epoch;
    return false;
  }
};

// One access point found by a Wi-Fi scan.
struct WifiNetwork {
  char ssid[33];
  int rssi;  // dBm
  uint8_t channel;
  bool secure;  // false = open network
};

// Drops hidden networks (empty SSID), keeps the strongest entry per SSID
// and sorts by signal, strongest first. Returns the new count.
int wifiScanNormalize(WifiNetwork* list, int n);

class Network {
 public:
  virtual ~Network() {}
  // Lists nearby access points (blocking, a few seconds). Returns the count
  // written to out, or -1 when the platform cannot scan.
  virtual int scan(WifiNetwork* out, int max) {
    (void)out;
    (void)max;
    return -1;
  }
  // Brings up the station link. Must give up after timeoutMs.
  virtual bool connect(const Config& cfg, uint32_t timeoutMs, char* err,
                       size_t errCap) = 0;
  virtual void disconnect() = 0;
  // Obtains wall clock time (SNTP); bounded by timeoutMs.
  virtual bool syncTime(uint32_t timeoutMs) = 0;
  virtual int rssi() = 0;               // dBm, 0 = unknown
  virtual const char* stateText() = 0;  // "off", "connecting", "connected", ...
  // Station IPv4 address while connected, "" otherwise.
  virtual const char* ipText() { return ""; }
};

struct HttpResult {
  bool transportOk;      // request sent and a status line received
  int status;            // HTTP status, -1 if none
  char error[64];        // transport error text (never contains secrets)
  uint32_t serverEpoch;  // HTTP Date header of the response, 0 = none
};

// Receives a streamed download (remote firmware update).
class DownloadSink {
 public:
  virtual ~DownloadSink() {}
  // Called once with Content-Length (0 = unknown) before any data.
  virtual bool begin(size_t contentLength) = 0;
  // Return false to abort the transfer.
  virtual bool data(const uint8_t* bytes, size_t len) = 0;
};

class HttpClient {
 public:
  virtual ~HttpClient() {}
  // POST application/json. TLS certificate validation is mandatory for
  // https:// URLs. The response body is truncated to respCap-1 bytes.
  virtual HttpResult postJson(const char* url, const char* body, size_t len,
                              uint32_t timeoutMs, char* resp,
                              size_t respCap) = 0;
  // GET a small text document (update manifest).
  virtual HttpResult get(const char* url, uint32_t timeoutMs, char* resp,
                         size_t respCap) = 0;
  // GET streamed into sink; timeoutMs bounds each network operation.
  virtual HttpResult download(const char* url, uint32_t timeoutMs,
                              DownloadSink& sink) = 0;
  // Requests within one upload window may share a connection (keep-alive,
  // one TLS handshake); endSession() closes it before the radio goes off.
  virtual void endSession() {}
};

class LogSource {
 public:
  virtual ~LogSource() {}
  // Writes bounded JSON {next,lost,lines}. since = last delivered sequence,
  // initially 0. false means unavailable/allocation failure or insufficient
  // cap.
  virtual bool read(uint64_t since, char* out, size_t cap) = 0;
};

class SystemPort {
 public:
  virtual ~SystemPort() {}
  virtual LogSource* logSource() { return nullptr; }
  virtual const char* platformName() = 0;  // "esp32c6" / "pc-sim"
  virtual int batteryMv() = 0;             // -1 if not available
  // Consumption from measured operating times, in 0.01 mAh/day.
  // -1 when unavailable or less than one hour has been observed.
  virtual int32_t batteryMahPerDayCenti() { return -1; }
  virtual const char* wakeReason() = 0;
  virtual const char* resetReason() = 0;
  virtual void requestRestart() = 0;  // performed after the reply
  // Platform specific extras for /api/status and /api/diag (OTA partitions,
  // heap, simulation state...). Must write complete key/value pairs into an
  // already opened object.
  virtual void writePlatformJson(JsonWriter& w, bool detailed) = 0;
  // Optional actions: "exit_service" (ESP32), "sim" (PC fault injection).
  // Returns false with msg when unsupported or failed.
  virtual bool platformAction(const char* action, const Form& args, char* msg,
                              size_t msgCap) = 0;
  // Version of a firmware that failed its first boot and was rolled back
  // ("" if none). Remote updates never retry that version.
  virtual const char* rejectedFirmware() { return ""; }
  // Board temperature in 1/100 °C (ESP32: chip sensor right after wake, close
  // to the enclosure temperature). false = not available.
  virtual bool boardTempCenti(int16_t* out) {
    (void)out;
    return false;
  }
  // Called after the configuration was loaded or changed (ESP32: UDP log
  // forwarding, LAN console wake). Runs with the application lock held.
  virtual void configApplied(const Config& cfg) { (void)cfg; }
};

}  // namespace gm

#endif
