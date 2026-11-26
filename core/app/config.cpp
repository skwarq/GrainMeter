// SPDX-License-Identifier: MIT
#include "app/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gm {

static const uint32_t kDefaultMeasureS = 15 * 60;
static const uint32_t kDefaultUploadS = 60 * 60;
static const uint32_t kMaxIntervalS = 7 * 24 * 3600;
static const uint32_t kMinUploadS =
    15;  // ThingSpeak: >= 15 s between bulk calls

uint32_t Config::samplesPerUpload() const {
  if (measureIntervalS == 0) {
    return 1;
  }
  const uint32_t n = uploadIntervalS / measureIntervalS;
  return n == 0 ? 1 : n;
}

static void copyStr(char* dst, size_t cap, const char* src) {
  snprintf(dst, cap, "%s", src);
}

void configDefaults(Config* cfg, const char* defaultReportUrl,
                    const char* defaultUpdateUrl) {
  memset(cfg, 0, sizeof(*cfg));
  copyStr(cfg->deviceName, sizeof(cfg->deviceName), "GrainMeter");
  copyStr(cfg->reportUrl, sizeof(cfg->reportUrl), defaultReportUrl);
  cfg->measureIntervalS = kDefaultMeasureS;
  cfg->uploadIntervalS = kDefaultUploadS;
  cfg->diagEveryH = 6;
  // AKYGA LP475085 LiPo (docs/hardware.md): ~8 % / ~4 %. Critical stops the
  // radio before Wi-Fi current peaks can brown the board out in the cold.
  cfg->battLowMv = 3650;   // economy mode
  cfg->battCritMv = 3500;  // measure only, no radio
  cfg->battCapacityMah = 2500;
  cfg->updateCheckH = 24;
  cfg->logUdp = 0;
  cfg->logDest = 0;
  cfg->logLevel = 3;
  cfg->logPort = 5514;
  cfg->lanWake = 1;
  cfg->remoteBudgetMin = 60;
  cfg->remoteIdleMin = 10;
  cfg->apEnabled = 1;
  cfg->apDelayS = 60;
  cfg->alarmRiseH = 24;
  cfg->alarmRepeatMin = 180;
  copyStr(cfg->updateUrl, sizeof(cfg->updateUrl),
          defaultUpdateUrl != nullptr ? defaultUpdateUrl : "");
}

// ---- validation helpers ----------------------------------------------------

static bool validText(const char* s, size_t minLen, size_t maxLen,
                      bool allowSpace) {
  const size_t n = strlen(s);
  if (n < minLen || n > maxLen) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c > 0x7E || (!allowSpace && c == ' ')) {
      return false;
    }
  }
  return true;
}

static bool validKey(const char* s) {
  const size_t n = strlen(s);
  if (n < 8 || n > 32) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const char c = s[i];
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z');
    if (!alnum) {
      return false;
    }
  }
  return true;
}

static bool validUrl(const char* s, bool allowInsecure, size_t maxLen = 95) {
  if (!validText(s, 9, maxLen, false)) {
    return false;
  }
  if (strncmp(s, "https://", 8) == 0) {
    return true;
  }
  return allowInsecure &&
         (strncmp(s, "http://", 7) == 0 || strncmp(s, "mock://", 7) == 0);
}

static bool validRemoteId(const char* s) {
  if (strlen(s) > 24) return false;
  for (; *s; ++s) {
    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-'))
      return false;
  }
  return true;
}

static bool validRemoteUrl(const char* s) {
  if (!*s) return true;
  if (!validText(s, 9, 96, false) || strncmp(s, "mqtts://", 8) != 0)
    return false;
  const char* host = s + 8;
  const char* port = strchr(host, ':');
  const char* end = port ? port : host + strlen(host);
  if (host == end || *host == '.' || *host == '-' || end[-1] == '.' ||
      end[-1] == '-')
    return false;
  for (const char* p = host; p < end; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
      return false;
  }
  if (port) {
    uint32_t n = 0;
    if (!*++port) return false;
    for (; *port; ++port) {
      if (*port < '0' || *port > '9') return false;
      n = n * 10 + static_cast<unsigned>(*port - '0');
      if (n > 65535) return false;
    }
    if (!n) return false;
  }
  return true;
}

// A topic URL has one path component; JSON publishing goes to the origin.
static bool validNtfy(const char* s, bool insecure) {
  if (!s[0]) return true;
  if (!validUrl(s, insecure, 159)) return false;
  const char* scheme = strstr(s, "://");
  const char* path = strchr(scheme + 3, '/');
  return path && path > scheme + 3 && path[1] &&
         strpbrk(path + 1, "/?#") == nullptr && strchr(s, '@') == nullptr;
}

static bool validToken(const char* s) {
  if (!*s) return true;
  if (!strchr(s, ':') || strlen(s) > 95) return false;
  for (; *s; ++s) {
    if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') ||
          (*s >= 'A' && *s <= 'Z') || *s == ':' || *s == '_' || *s == '-'))
      return false;
  }
  return true;
}

static bool parseCenti(const char* s, uint32_t max, uint32_t* out) {
  uint32_t whole = 0, frac = 0, digits = 0;
  if (*s < '0' || *s > '9') return false;
  while (*s >= '0' && *s <= '9') {
    whole = whole * 10 + static_cast<unsigned>(*s++ - '0');
    if (whole > max) return false;
  }
  if (*s == '.') {
    ++s;
    while (*s >= '0' && *s <= '9' && digits < 2) {
      frac = frac * 10 + static_cast<unsigned>(*s++ - '0');
      ++digits;
    }
    if (!digits) return false;
  }
  if (*s) return false;
  if (digits == 1) frac *= 10;
  if (whole * 100 + frac > max * 100) return false;
  *out = whole * 100 + frac;
  return true;
}

bool parseIpv4(const char* s, uint8_t out[4]) {
  int part = 0;
  unsigned value = 0;
  int digits = 0;
  for (const char* p = s;; ++p) {
    if (*p >= '0' && *p <= '9') {
      value = value * 10u + static_cast<unsigned>(*p - '0');
      if (++digits > 3 || value > 255) return false;
    } else if (*p == '.' || *p == '\0') {
      if (digits == 0 || part > 3) return false;
      out[part++] = static_cast<uint8_t>(value);
      value = 0;
      digits = 0;
      if (*p == '\0') break;
    } else {
      return false;
    }
  }
  return part == 4;
}

static bool validIpOrEmpty(const char* s) {
  uint8_t ip[4];
  return s[0] == '\0' || parseIpv4(s, ip);
}

static bool validBattMv(uint32_t v) {
  return v == 0 || (v >= 2500 && v <= 4500);
}

static bool parseU32(const char* s, uint32_t lo, uint32_t hi, uint32_t* out) {
  if (s[0] == '\0') {
    return false;
  }
  uint32_t value = 0;
  for (const char* p = s; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(*p - '0');
    if (value > (UINT32_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  if (value < lo || value > hi) return false;
  *out = value;
  return true;
}

static void setErr(char* err, size_t cap, const char* msg) {
  if (err != nullptr && cap > 0) {
    copyStr(err, cap, msg);
  }
}

// ---- form ------------------------------------------------------------------

bool configApplyForm(const Form& form, const ConfigLimits& limits, Config* cfg,
                     char* err, size_t errCap) {
  Config next = *cfg;
  char v[192];

  if (form.has("name")) {
    if (!form.get("name", v, sizeof(v)) ||
        !validText(v, 1, sizeof(next.deviceName) - 1, true)) {
      setErr(err, errCap, "name: 1..31 printable characters");
      return false;
    }
    copyStr(next.deviceName, sizeof(next.deviceName), v);
  }
  if (form.has("wifi_ssid")) {
    if (!form.get("wifi_ssid", v, sizeof(v)) || !validText(v, 0, 32, true)) {
      setErr(err, errCap, "wifi_ssid: 0..32 printable characters");
      return false;
    }
    copyStr(next.wifiSsid, sizeof(next.wifiSsid), v);
  }
  if (form.has("wifi_pass")) {
    if (!form.get("wifi_pass", v, sizeof(v))) {
      setErr(err, errCap, "wifi_pass: invalid encoding");
      return false;
    }
    if (v[0] != '\0') {  // empty = keep current
      if (!validText(v, 8, 63, true)) {
        setErr(err, errCap, "wifi_pass: 8..63 printable characters");
        return false;
      }
      copyStr(next.wifiPass, sizeof(next.wifiPass), v);
    }
  }
  if (form.has("wifi_pass_clear")) {
    next.wifiPass[0] = '\0';  // open network
  }
  if (form.has("report_url")) {
    if (!form.get("report_url", v, sizeof(v))) {
      setErr(err, errCap, "report_url: invalid encoding");
      return false;
    }
    size_t n = strlen(v);
    while (n > 0 && v[n - 1] == '/') {
      v[--n] = '\0';
    }
    if (!validUrl(v, limits.allowInsecureUrl)) {
      setErr(err, errCap,
             limits.allowInsecureUrl
                 ? "report_url: https://, http:// or mock:// URL"
                 : "report_url: must be an https:// URL");
      return false;
    }
    copyStr(next.reportUrl, sizeof(next.reportUrl), v);
  }
  const struct {
    const char* key;
    char* dst;
    size_t cap;
    bool secret;
    int kind;
  } remote[] = {
      {"remote_url", next.remoteUrl, sizeof(next.remoteUrl), false, 1},
      {"remote_user", next.remoteUser, sizeof(next.remoteUser), false, 0},
      {"remote_pass", next.remotePass, sizeof(next.remotePass), true, 0},
      {"remote_id", next.remoteId, sizeof(next.remoteId), false, 2},
      {"talkback_key", next.talkbackKey, sizeof(next.talkbackKey), true, 0},
  };
  for (const auto& f : remote) {
    if (!form.has(f.key)) continue;
    if (!form.get(f.key, v, sizeof(v)) || !validText(v, 0, f.cap - 1, true) ||
        (f.kind == 1 && !validRemoteUrl(v)) ||
        (f.kind == 2 && !validRemoteId(v))) {
      setErr(err, errCap, "remote configuration: invalid value");
      return false;
    }
    if (!f.secret || *v) copyStr(f.dst, f.cap, v);
  }
  if (form.has("remote_pass_clear")) next.remotePass[0] = 0;
  if (form.has("talkback_key_clear")) next.talkbackKey[0] = 0;
  struct U32Field {
    const char* name;
    uint32_t* dst;
    uint32_t lo;
    uint32_t hi;
    const char* msg;
  };
  const U32Field u32s[] = {
      {"ts_channel", &next.channelId, 0, 99999999u, "ts_channel: 0..99999999"},
      {"diag_channel", &next.diagChannelId, 0, 99999999u,
       "diag_channel: 0..99999999"},
      {"measure_s", &next.measureIntervalS, limits.minMeasureS, kMaxIntervalS,
       "measure_s out of range"},
      {"upload_s", &next.uploadIntervalS, kMinUploadS, kMaxIntervalS,
       "upload_s out of range"},
  };
  for (size_t i = 0; i < sizeof(u32s) / sizeof(u32s[0]); ++i) {
    if (!form.has(u32s[i].name)) {
      continue;
    }
    if (!form.get(u32s[i].name, v, sizeof(v)) ||
        !parseU32(v, u32s[i].lo, u32s[i].hi, u32s[i].dst)) {
      setErr(err, errCap, u32s[i].msg);
      return false;
    }
  }
  struct KeyField {
    const char* name;
    char* dst;
    size_t cap;
  };
  const KeyField keys[] = {
      {"ts_key", next.writeKey, sizeof(next.writeKey)},
      {"diag_key", next.diagWriteKey, sizeof(next.diagWriteKey)},
  };
  for (size_t i = 0; i < 2; ++i) {
    if (!form.has(keys[i].name)) {
      continue;
    }
    if (!form.get(keys[i].name, v, sizeof(v))) {
      setErr(err, errCap, "API key: invalid encoding");
      return false;
    }
    if (v[0] == '\0') {
      continue;  // keep current
    }
    if (!validKey(v)) {
      setErr(err, errCap, "API key: 8..32 letters/digits");
      return false;
    }
    copyStr(keys[i].dst, keys[i].cap, v);
  }
  if (form.has("svc_pass")) {
    if (!form.get("svc_pass", v, sizeof(v))) {
      setErr(err, errCap, "svc_pass: invalid encoding");
      return false;
    }
    if (v[0] != '\0') {
      if (!validText(v, 8, 63, false)) {
        setErr(err, errCap, "svc_pass: 8..63 printable characters, no spaces");
        return false;
      }
      copyStr(next.servicePassword, sizeof(next.servicePassword), v);
    }
  }
  struct U32Field2 {
    const char* name;
    uint32_t* dst;
    uint32_t lo;
    uint32_t hi;
    const char* msg;
  };
  const U32Field2 more[] = {
      {"alarm_rise_h", &next.alarmRiseH, 1, 72, "alarm_rise_h: 1..72"},
      {"alarm_repeat_min", &next.alarmRepeatMin, 0, 1440,
       "alarm_repeat_min: 0..1440"},
      {"diag_every_h", &next.diagEveryH, 0, 168, "diag_every_h: 0..168"},
      {"batt_low_mv", &next.battLowMv, 0, 4500,
       "batt_low_mv: 0 (off) or 2500..4500"},
      {"batt_crit_mv", &next.battCritMv, 0, 4500,
       "batt_crit_mv: 0 (off) or 2500..4500"},
      {"batt_capacity_mah", &next.battCapacityMah, 100, 20000,
       "batt_capacity_mah: 100..20000"},
      {"update_check_h", &next.updateCheckH, 0, 720, "update_check_h: 0..720"},
      {"log_udp", &next.logUdp, 0, 1, "log_udp: 0 or 1"},
      {"log_level", &next.logLevel, 1, 4,
       "log_level: 1 error, 2 warning, 3 info, 4 debug"},
      {"log_port", &next.logPort, 1, 65535, "log_port: 1..65535"},
      {"talkback_id", &next.talkbackId, 0, UINT32_MAX, "talkback_id: uint32"},
      {"remote_budget_min", &next.remoteBudgetMin, 0, 1440,
       "remote_budget_min: 0..1440"},
      {"remote_idle_min", &next.remoteIdleMin, 1, 60, "remote_idle_min: 1..60"},
      {"lan_wake", &next.lanWake, 0, 1, "lan_wake: 0 or 1"},
      {"ap_enabled", &next.apEnabled, 0, 1, "ap_enabled: 0 or 1"},
      {"ap_delay_s", &next.apDelayS, 0, 3600, "ap_delay_s: 0..3600"},
  };
  for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); ++i) {
    if (!form.has(more[i].name)) {
      continue;
    }
    if (!form.get(more[i].name, v, sizeof(v)) ||
        !parseU32(v, more[i].lo, more[i].hi, more[i].dst)) {
      setErr(err, errCap, more[i].msg);
      return false;
    }
  }
  if (!validBattMv(next.battLowMv) || !validBattMv(next.battCritMv)) {
    setErr(err, errCap, "battery thresholds: 0 (off) or 2500..4500 mV");
    return false;
  }
  if (next.battLowMv != 0 && next.battCritMv > next.battLowMv) {
    setErr(err, errCap, "batt_crit_mv must be <= batt_low_mv");
    return false;
  }
  if (form.has("update_url")) {
    char url[sizeof(next.updateUrl)];  // longer input is rejected by get()
    if (!form.get("update_url", url, sizeof(url))) {
      setErr(err, errCap, "update_url: invalid encoding or too long");
      return false;
    }
    if (url[0] != '\0' &&
        !validUrl(url, limits.allowInsecureUrl, sizeof(next.updateUrl) - 1)) {
      setErr(err, errCap,
             limits.allowInsecureUrl
                 ? "update_url: empty or https://, http://, mock:// URL"
                 : "update_url: empty or an https:// URL");
      return false;
    }
    copyStr(next.updateUrl, sizeof(next.updateUrl), url);
  }
  struct IpField {
    const char* name;
    char* dst;
  };
  const IpField ips[] = {{"static_ip", next.staticIp},
                         {"gateway", next.gateway},
                         {"netmask", next.netmask},
                         {"dns", next.dns}};
  for (size_t i = 0; i < 4; ++i) {
    if (!form.has(ips[i].name)) {
      continue;
    }
    if (!form.get(ips[i].name, v, sizeof(v)) || strlen(v) > 15 ||
        !validIpOrEmpty(v)) {
      setErr(err, errCap, "IP fields: empty or a.b.c.d");
      return false;
    }
    copyStr(ips[i].dst, 16, v);
  }
  const struct {
    const char* key;
    uint32_t* dst;
    uint32_t max;
  } temps[] = {{"alarm_temp_c", &next.alarmTempCenti, 80},
               {"alarm_rise_c", &next.alarmRiseCenti, 20}};
  for (const auto& f : temps) {
    if (form.has(f.key) &&
        (!form.get(f.key, v, sizeof(v)) || !parseCenti(v, f.max, f.dst))) {
      setErr(err, errCap, "alarm temperature out of range");
      return false;
    }
  }
  if (form.has("notify_ntfy_url")) {
    if (!form.get("notify_ntfy_url", v, sizeof(v)) ||
        !validNtfy(v, limits.allowInsecureUrl)) {
      setErr(err, errCap, "notify_ntfy_url: empty or server/topic URL");
      return false;
    }
    copyStr(next.notifyNtfyUrl, sizeof(next.notifyNtfyUrl), v);
  }
  if (form.has("notify_tg_token")) {
    if (!form.get("notify_tg_token", v, sizeof(v)) || !validToken(v)) {
      setErr(err, errCap, "notify_tg_token: invalid token");
      return false;
    }
    if (*v) copyStr(next.notifyTgToken, sizeof(next.notifyTgToken), v);
  }
  if (form.has("notify_tg_token_clear")) next.notifyTgToken[0] = 0;
  if (form.has("notify_tg_chat")) {
    if (!form.get("notify_tg_chat", v, sizeof(v)) ||
        !validText(v, 0, 63, false)) {
      setErr(err, errCap, "notify_tg_chat: invalid chat id");
      return false;
    }
    copyStr(next.notifyTgChat, sizeof(next.notifyTgChat), v);
  }
  if (form.has("log_dest")) {
    if (!form.get("log_dest", v, sizeof(v)) ||
        (strcmp(v, "udp") != 0 && strcmp(v, "mqtt") != 0)) {
      setErr(err, errCap, "log_dest: udp or mqtt");
      return false;
    }
    next.logDest = strcmp(v, "mqtt") == 0 ? 1 : 0;
  }
  if (form.has("log_host")) {
    if (!form.get("log_host", v, sizeof(v)) || strlen(v) > 15 ||
        !validIpOrEmpty(v)) {
      setErr(err, errCap, "log_host: empty (broadcast) or a.b.c.d");
      return false;
    }
    copyStr(next.logHost, sizeof(next.logHost), v);
  }
  if (next.staticIp[0] != '\0' &&
      (next.gateway[0] == '\0' || next.netmask[0] == '\0')) {
    setErr(err, errCap, "static_ip needs gateway and netmask");
    return false;
  }
  if (next.uploadIntervalS < next.measureIntervalS) {
    setErr(err, errCap, "upload_s must be >= measure_s");
    return false;
  }
  *cfg = next;
  setErr(err, errCap, "");
  return true;
}

// ---- persistence
// -------------------------------------------------------------

bool configLoad(KeyValueStore& store, Config* cfg, RomMap* map,
                const char** err) {
  bool ok = true;
  char v[256];
  *err = nullptr;
  struct TextKey {
    const char* key;
    char* dst;
    size_t cap;
    int kind;  // 0 text, 1 secret, 2 API key, 3 URL, 4 URL/empty, 5 IP, 6 ntfy,
               // 7 token
  };
  const TextKey texts[] = {
      {"remote_url", cfg->remoteUrl, sizeof(cfg->remoteUrl), 8},
      {"remote_user", cfg->remoteUser, sizeof(cfg->remoteUser), 0},
      {"remote_pass", cfg->remotePass, sizeof(cfg->remotePass), 1},
      {"remote_id", cfg->remoteId, sizeof(cfg->remoteId), 9},
      {"talkback_key", cfg->talkbackKey, sizeof(cfg->talkbackKey), 1},
      {"notify_ntfy_url", cfg->notifyNtfyUrl, sizeof(cfg->notifyNtfyUrl), 6},
      {"notify_tg_token", cfg->notifyTgToken, sizeof(cfg->notifyTgToken), 7},
      {"notify_tg_chat", cfg->notifyTgChat, sizeof(cfg->notifyTgChat), 0},
      {"name", cfg->deviceName, sizeof(cfg->deviceName), 0},
      {"wifi_ssid", cfg->wifiSsid, sizeof(cfg->wifiSsid), 0},
      {"wifi_pass", cfg->wifiPass, sizeof(cfg->wifiPass), 1},
      {"report_url", cfg->reportUrl, sizeof(cfg->reportUrl), 3},
      {"ts_key", cfg->writeKey, sizeof(cfg->writeKey), 2},
      {"diag_key", cfg->diagWriteKey, sizeof(cfg->diagWriteKey), 2},
      {"svc_pass", cfg->servicePassword, sizeof(cfg->servicePassword), 1},
      {"update_url", cfg->updateUrl, sizeof(cfg->updateUrl), 4},
      {"static_ip", cfg->staticIp, sizeof(cfg->staticIp), 5},
      {"gateway", cfg->gateway, sizeof(cfg->gateway), 5},
      {"netmask", cfg->netmask, sizeof(cfg->netmask), 5},
      {"dns", cfg->dns, sizeof(cfg->dns), 5},
      {"log_host", cfg->logHost, sizeof(cfg->logHost), 5},
  };
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
    if (!store.get(texts[i].key, v, sizeof(v))) {
      continue;
    }
    bool valid;
    switch (texts[i].kind) {
      case 8:
        valid = validRemoteUrl(v);
        break;
      case 9:
        valid = validRemoteId(v);
        break;
      case 6:
        valid = validNtfy(v, true);
        break;
      case 7:
        valid = validToken(v);
        break;
      case 2:
        valid = v[0] == '\0' || validKey(v);
        break;
      case 3:
        valid = validUrl(v, true);
        break;  // platform re-checks scheme
      case 4:
        valid = v[0] == '\0' || validUrl(v, true, texts[i].cap - 1);
        break;
      case 5:
        valid = strlen(v) < texts[i].cap && validIpOrEmpty(v);
        break;
      default:
        valid = validText(v, 0, texts[i].cap - 1, true);
        break;
    }
    if (valid) {
      copyStr(texts[i].dst, texts[i].cap, v);
    } else {
      ok = false;
      *err = "stored configuration value invalid; default kept";
    }
  }
  struct NumKey {
    const char* key;
    uint32_t* dst;
    uint32_t lo;
    uint32_t hi;
  };
  const NumKey nums[] = {
      {"ts_channel", &cfg->channelId, 0, 99999999u},
      {"diag_channel", &cfg->diagChannelId, 0, 99999999u},
      {"measure_s", &cfg->measureIntervalS, 1, kMaxIntervalS},
      {"upload_s", &cfg->uploadIntervalS, kMinUploadS, kMaxIntervalS},
      {"alarm_rise_h", &cfg->alarmRiseH, 1, 72},
      {"alarm_repeat_m", &cfg->alarmRepeatMin, 0, 1440},
      {"diag_every_h", &cfg->diagEveryH, 0, 168},
      {"batt_low_mv", &cfg->battLowMv, 0, 4500},
      {"batt_crit_mv", &cfg->battCritMv, 0, 4500},
      {"batt_capacity", &cfg->battCapacityMah, 100, 20000},
      {"update_check_h", &cfg->updateCheckH, 0, 720},
      {"log_udp", &cfg->logUdp, 0, 1},
      {"log_dest", &cfg->logDest, 0, 1},
      {"log_level", &cfg->logLevel, 1, 4},
      {"log_port", &cfg->logPort, 1, 65535},
      {"talkback_id", &cfg->talkbackId, 0, UINT32_MAX},
      {"remote_budget_m", &cfg->remoteBudgetMin, 0, 1440},
      {"remote_idle_min", &cfg->remoteIdleMin, 1, 60},
      {"lan_wake", &cfg->lanWake, 0, 1},
      {"ap_enabled", &cfg->apEnabled, 0, 1},
      {"ap_delay_s", &cfg->apDelayS, 0, 3600},
  };
  for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); ++i) {
    if (store.get(nums[i].key, v, sizeof(v)) &&
        !parseU32(v, nums[i].lo, nums[i].hi, nums[i].dst)) {
      ok = false;
      *err = "stored configuration value invalid; default kept";
    }
  }
  const struct {
    const char* key;
    uint32_t* dst;
    uint32_t max;
  } temps[] = {{"alarm_temp_c", &cfg->alarmTempCenti, 80},
               {"alarm_rise_c", &cfg->alarmRiseCenti, 20}};
  for (const auto& f : temps) {
    if (store.get(f.key, v, sizeof(v)) && !parseCenti(v, f.max, f.dst)) {
      ok = false;
      *err = "stored alarm temperature invalid; default kept";
    }
  }
  if (cfg->uploadIntervalS < cfg->measureIntervalS) {
    cfg->uploadIntervalS = cfg->measureIntervalS;
    ok = false;
    *err = "upload interval shorter than measure interval; corrected";
  }
  if (store.get("rommap", v, sizeof(v)) && !map->parse(v)) {
    ok = false;
    *err = "stored ROM map invalid; ignored";
  }
  return ok;
}

bool configSave(KeyValueStore& store, const Config& cfg) {
  char num[16];
  bool ok = true;
  ok = store.set("notify_ntfy_url", cfg.notifyNtfyUrl) && ok;
  ok = store.set("notify_tg_token", cfg.notifyTgToken) && ok;
  ok = store.set("notify_tg_chat", cfg.notifyTgChat) && ok;
  snprintf(num, sizeof(num), "%u.%02u",
           static_cast<unsigned>(cfg.alarmTempCenti / 100),
           static_cast<unsigned>(cfg.alarmTempCenti % 100));
  ok = store.set("alarm_temp_c", num) && ok;
  snprintf(num, sizeof(num), "%u.%02u",
           static_cast<unsigned>(cfg.alarmRiseCenti / 100),
           static_cast<unsigned>(cfg.alarmRiseCenti % 100));
  ok = store.set("alarm_rise_c", num) && ok;
  ok = store.set("name", cfg.deviceName) && ok;
  ok = store.set("wifi_ssid", cfg.wifiSsid) && ok;
  ok = store.set("wifi_pass", cfg.wifiPass) && ok;
  ok = store.set("report_url", cfg.reportUrl) && ok;
  snprintf(num, sizeof(num), "%lu", static_cast<unsigned long>(cfg.channelId));
  ok = store.set("ts_channel", num) && ok;
  ok = store.set("ts_key", cfg.writeKey) && ok;
  snprintf(num, sizeof(num), "%lu",
           static_cast<unsigned long>(cfg.diagChannelId));
  ok = store.set("diag_channel", num) && ok;
  ok = store.set("diag_key", cfg.diagWriteKey) && ok;
  snprintf(num, sizeof(num), "%lu",
           static_cast<unsigned long>(cfg.measureIntervalS));
  ok = store.set("measure_s", num) && ok;
  snprintf(num, sizeof(num), "%lu",
           static_cast<unsigned long>(cfg.uploadIntervalS));
  ok = store.set("upload_s", num) && ok;
  ok = store.set("svc_pass", cfg.servicePassword) && ok;
  const struct {
    const char* key;
    uint32_t value;
  } nums[] = {{"alarm_rise_h", cfg.alarmRiseH},
              {"alarm_repeat_m", cfg.alarmRepeatMin},
              {"diag_every_h", cfg.diagEveryH},
              {"batt_low_mv", cfg.battLowMv},
              {"batt_crit_mv", cfg.battCritMv},
              {"update_check_h", cfg.updateCheckH},
              {"batt_capacity", cfg.battCapacityMah},
              {"log_udp", cfg.logUdp},
              {"log_dest", cfg.logDest},
              {"log_level", cfg.logLevel},
              {"log_port", cfg.logPort},
              {"lan_wake", cfg.lanWake},
              {"ap_enabled", cfg.apEnabled},
              {"ap_delay_s", cfg.apDelayS}};
  for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); ++i) {
    snprintf(num, sizeof(num), "%lu",
             static_cast<unsigned long>(nums[i].value));
    ok = store.set(nums[i].key, num) && ok;
  }
  ok = store.set("update_url", cfg.updateUrl) && ok;
  ok = store.set("static_ip", cfg.staticIp) && ok;
  ok = store.set("gateway", cfg.gateway) && ok;
  ok = store.set("netmask", cfg.netmask) && ok;
  ok = store.set("dns", cfg.dns) && ok;
  ok = store.set("log_host", cfg.logHost) && ok;
  ok = store.set("remote_url", cfg.remoteUrl) && ok;
  ok = store.set("remote_user", cfg.remoteUser) && ok;
  ok = store.set("remote_pass", cfg.remotePass) && ok;
  ok = store.set("remote_id", cfg.remoteId) && ok;
  ok = store.set("talkback_key", cfg.talkbackKey) && ok;
  snprintf(num, sizeof(num), "%lu", static_cast<unsigned long>(cfg.talkbackId));
  ok = store.set("talkback_id", num) && ok;
  snprintf(num, sizeof(num), "%lu",
           static_cast<unsigned long>(cfg.remoteBudgetMin));
  ok = store.set("remote_budget_m", num) && ok;
  snprintf(num, sizeof(num), "%lu",
           static_cast<unsigned long>(cfg.remoteIdleMin));
  ok = store.set("remote_idle_min", num) && ok;
  return store.commit() && ok;
}

bool romMapSave(KeyValueStore& store, const RomMap& map) {
  char text[256];
  if (!map.serialize(text, sizeof(text))) {
    return false;
  }
  return store.set("rommap", text) && store.commit();
}

void configWriteJson(JsonWriter& w, const Config& cfg) {
  w.beginObject()
      .key("alarm_temp_c")
      .centi(static_cast<int16_t>(cfg.alarmTempCenti))
      .key("alarm_rise_c")
      .centi(static_cast<int16_t>(cfg.alarmRiseCenti))
      .kvu("alarm_rise_h", cfg.alarmRiseH)
      .kvu("alarm_repeat_min", cfg.alarmRepeatMin)
      .kv("notify_ntfy_url", cfg.notifyNtfyUrl)
      .kvb("notify_tg_token_set", cfg.notifyTgToken[0] != 0)
      .kv("notify_tg_chat", cfg.notifyTgChat)
      .kv("name", cfg.deviceName)
      .kv("wifi_ssid", cfg.wifiSsid)
      .kvb("wifi_pass_set", cfg.wifiPass[0] != '\0')
      .kv("report_url", cfg.reportUrl)
      .kvu("ts_channel", cfg.channelId)
      .kvb("ts_key_set", cfg.writeKey[0] != '\0')
      .kvu("diag_channel", cfg.diagChannelId)
      .kvb("diag_key_set", cfg.diagWriteKey[0] != '\0')
      .kvu("measure_s", cfg.measureIntervalS)
      .kvu("upload_s", cfg.uploadIntervalS)
      .kvu("samples_per_upload", cfg.samplesPerUpload())
      .kvb("svc_pass_set", cfg.servicePassword[0] != '\0')
      .kvu("diag_every_h", cfg.diagEveryH)
      .kvu("batt_low_mv", cfg.battLowMv)
      .kvu("batt_crit_mv", cfg.battCritMv)
      .kvu("batt_capacity_mah", cfg.battCapacityMah)
      .kv("update_url", cfg.updateUrl)
      .kvu("update_check_h", cfg.updateCheckH)
      .kv("static_ip", cfg.staticIp)
      .kv("gateway", cfg.gateway)
      .kv("netmask", cfg.netmask)
      .kv("dns", cfg.dns)
      .kvb("log_udp", cfg.logUdp != 0)
      .kv("log_dest", cfg.logDest == 1 ? "mqtt" : "udp")
      .kvu("log_level", cfg.logLevel)
      .kv("log_host", cfg.logHost)
      .kvu("log_port", cfg.logPort)
      .kv("remote_url", cfg.remoteUrl)
      .kv("remote_user", cfg.remoteUser)
      .kvb("remote_pass_set", cfg.remotePass[0] != 0)
      .kv("remote_id", cfg.remoteId)
      .kvu("talkback_id", cfg.talkbackId)
      .kvb("talkback_key_set", cfg.talkbackKey[0] != 0)
      .kvu("remote_budget_min", cfg.remoteBudgetMin)
      .kvu("remote_idle_min", cfg.remoteIdleMin)
      .kvb("lan_wake", cfg.lanWake != 0)
      .kvb("ap_enabled", cfg.apEnabled != 0)
      .kvu("ap_delay_s", cfg.apDelayS)
      .endObject();
}

}  // namespace gm
