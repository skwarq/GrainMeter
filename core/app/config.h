// SPDX-License-Identifier: MIT
#ifndef GM_CONFIG_H
#define GM_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "data/rommap.h"
#include "util/form.h"
#include "util/json.h"

namespace gm {

// Persistent key/value storage. ESP32: NVS namespace "grainmeter".
// PC: local text file (default ./grainmeter-pc.conf, never in the repo).
class KeyValueStore {
 public:
  virtual ~KeyValueStore() {}
  virtual bool get(const char* key, char* out,
                   size_t cap) = 0;  // false if absent
  virtual bool set(const char* key, const char* value) = 0;
  virtual bool commit() = 0;
};

struct Config {
  char deviceName[32];
  char wifiSsid[33];
  char wifiPass[65];       // secret
  char reportUrl[96];      // base URL, e.g. https://api.thingspeak.com
  uint32_t channelId;      // main channel: 8 temperatures
  char writeKey[33];       // secret
  uint32_t diagChannelId;  // diagnostic channel (battery, RSSI, ...)
  char diagWriteKey[33];   // secret
  uint32_t measureIntervalS;
  uint32_t uploadIntervalS;
  char servicePassword[65];  // secret: SoftAP WPA2 + portal write access

  // Diagnostic channel only every N hours (0 = with every upload).
  uint32_t diagEveryH;
  // Battery protection thresholds in mV (0 = disabled), see powerModeFor().
  uint32_t battLowMv;
  uint32_t battCritMv;
  // Remote update manifest (https://..., empty = off), checked every N h.
  char updateUrl[160];
  uint32_t updateCheckH;
  // Static IPv4 for the station (empty = DHCP): shortens the Wi-Fi window.
  char staticIp[16];
  char gateway[16];
  char netmask[16];
  char dns[16];

  bool uploadConfigured() const {
    return channelId != 0 && writeKey[0] != '\0' && reportUrl[0] != '\0';
  }
  uint32_t samplesPerUpload() const;
  bool staticIpEnabled() const { return staticIp[0] != '\0'; }
  bool remoteUpdateEnabled() const {
    return updateUrl[0] != '\0' && updateCheckH != 0;
  }
};

// Parses dotted IPv4 "a.b.c.d" into network-order bytes.
bool parseIpv4(const char* s, uint8_t out[4]);

struct ConfigLimits {
  uint32_t minMeasureS;   // ESP32: 60, PC: 5 (accelerated tests)
  bool allowInsecureUrl;  // PC: http:// and mock:// allowed
};

void configDefaults(Config* cfg, const char* defaultReportUrl);

// Loads config + ROM map; missing keys keep defaults. Returns false if a
// stored value was invalid (it is then ignored and reported via *err).
bool configLoad(KeyValueStore& store, Config* cfg, RomMap* map,
                const char** err);
bool configSave(KeyValueStore& store, const Config& cfg);
bool romMapSave(KeyValueStore& store, const RomMap& map);

// Applies portal/console form fields. Secret fields left empty keep the
// current value. Validates everything first; on failure cfg is untouched and
// err describes the first problem and false is returned.
bool configApplyForm(const Form& form, const ConfigLimits& limits, Config* cfg,
                     char* err, size_t errCap);

// Writes the configuration without secrets (only "*_set" booleans).
void configWriteJson(JsonWriter& w, const Config& cfg);

}  // namespace gm

#endif
