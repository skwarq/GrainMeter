// SPDX-License-Identifier: MIT
#include "app/grainmeter.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "comm/thingspeak.h"
#include "onewire/ds18b20.h"
#include "ota/remote_update.h"
#include "util/log.h"
#include "util/sha256.h"

namespace gm {

static const char* kTag = "app";

GrainMeter::GrainMeter(const Ports& ports, RetainedState* state,
                       const ConfigLimits& limits, const char* defaultReportUrl,
                       const char* defaultUpdateUrl)
    : p_(ports),
      st_(state),
      limits_(limits),
      defaultReportUrl_(defaultReportUrl),
      defaultUpdateUrl_(defaultUpdateUrl),
      haveScan_(false),
      haveMeasure_(false),
      configOk_(true),
      stateRecovered_(false),
      powerMode_(PowerMode::Normal),
      ota_(*ports.ota, kChipIdEsp32C6, "grainmeter"),
      errorCount_(0),
      errorNext_(0) {
  memset(&scan_, 0, sizeof(scan_));
  memset(&meas_, 0, sizeof(meas_));
  memset(errors_, 0, sizeof(errors_));
  configDefaults(&cfg_, defaultReportUrl_, defaultUpdateUrl_);
}

bool GrainMeter::begin() {
  configDefaults(&cfg_, defaultReportUrl_, defaultUpdateUrl_);
  map_.clear();
  const char* err = nullptr;
  configOk_ = configLoad(*p_.store, &cfg_, &map_, &err);
  if (!configOk_) {
    recordError("config: %s", err != nullptr ? err : "load failed");
  }
  if (!st_->valid()) {
    // First boot, power loss (RTC memory lost) or corruption. Samples
    // that were not uploaded are gone; counters restart.
    st_->init();
    stateRecovered_ = true;
    GM_LOGW(kTag,
            "retained state invalid -> initialised (power-on or corruption)");
  }
  ++st_->wakeCount;
  st_->seal();
  GM_LOGI(
      kTag, "GrainMeter %s on %s, %u point(s) mapped, %u sample(s) buffered",
      GM_FW_VERSION, p_.sys->platformName(), static_cast<unsigned>(map_.size()),
      static_cast<unsigned>(st_->count));
  return configOk_;
}

void GrainMeter::recordError(const char* fmt, ...) {
  ErrorEntry& e = errors_[errorNext_];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof(e.text), fmt, ap);
  va_end(ap);
  e.monoS = p_.clock->monoS();
  errorNext_ = (errorNext_ + 1) % kErrorSlots;
  if (errorCount_ < kErrorSlots) {
    ++errorCount_;
  }
  GM_LOGW(kTag, "%s", e.text);
}

// ---------------------------------------------------------------- 1-Wire ----

const ScanReport& GrainMeter::scan() {
  memset(&scan_, 0, sizeof(scan_));
  scan_.search = searchRom(*p_.bus, scan_.roms, kMaxSensors);
  scan_.monoS = p_.clock->monoS();
  haveScan_ = true;
  const SearchResult& r = scan_.search;
  GM_LOGI(kTag,
          "scan: presence=%d found=%u crc_errors=%u bus_error=%d truncated=%d",
          r.presence, static_cast<unsigned>(r.found),
          static_cast<unsigned>(r.crcErrors), r.busError, r.truncated);
  for (size_t i = 0; i < r.found; ++i) {
    char hex[kRomTextLen + 1];
    romToHex(scan_.roms[i], hex, sizeof(hex));
    GM_LOGI(kTag, "  ROM %s family 0x%02X %s%s point=%u", hex,
            scan_.roms[i].family(), familyName(scan_.roms[i].family()),
            familySupported(scan_.roms[i].family()) ? "" : " (not measured)",
            map_.pointOf(scan_.roms[i]));
  }
  if (!r.presence) {
    recordError("1-Wire: no presence pulse (probe not connected?)");
  } else if (r.crcErrors > 0) {
    recordError("1-Wire: %u ROM code(s) with CRC error during search",
                static_cast<unsigned>(r.crcErrors));
  }
  return scan_;
}

static void addReading(MeasureReport& m, const Rom& rom, uint8_t point) {
  if (m.count >= kMaxSensors) {
    return;
  }
  SensorReading& r = m.readings[m.count++];
  r.rom = rom;
  r.point = point;
  r.status = ReadStatus::Missing;
  r.centi = 0;
}

const MeasureReport& GrainMeter::measure(bool store) {
  MeasureReport& m = meas_;
  memset(&m, 0, sizeof(m));
  scan();
  m.monoS = p_.clock->monoS();
  m.epoch = p_.clock->wallValid() ? p_.clock->epoch() : 0;
  m.presence = scan_.search.presence;
  m.crcRomErrors = scan_.search.crcErrors;

  if (m.presence && startConversionAll(*p_.bus)) {
    bool timedOut = false;
    m.conversionMs =
        waitConversion(*p_.bus, kConversionTimeMs12Bit + 100, &timedOut);
    m.conversionTimeout = timedOut;
    if (timedOut) {
      recordError("1-Wire: conversion not finished after %u ms",
                  static_cast<unsigned>(m.conversionMs));
    }
  }

  // Mapped sensors first (by point), then other devices found on the bus.
  for (size_t i = 0; i < map_.size(); ++i) {
    addReading(m, map_.at(i).rom, map_.at(i).point);
  }
  for (size_t i = 0; i < scan_.search.found; ++i) {
    if (map_.pointOf(scan_.roms[i]) == 0) {
      addReading(m, scan_.roms[i], 0);
    }
  }
  for (size_t i = 0; i < m.count; ++i) {
    SensorReading& r = m.readings[i];
    if (!familySupported(r.rom.family())) {
      r.status = ReadStatus::Unsupported;
    } else if (!m.presence) {
      r.status = ReadStatus::Missing;
    } else {
      r.status = readSensor(*p_.bus, r.rom, &r.centi);
    }
    if (r.point != 0 && r.status != ReadStatus::Ok) {
      char hex[kRomTextLen + 1];
      recordError("point %u (%s): %s", r.point,
                  romToHex(r.rom, hex, sizeof(hex)), readStatusName(r.status));
    }
  }
  haveMeasure_ = true;

  if (store) {
    st_->hasMeasured = 1;
    st_->lastMeasureMonoS = m.monoS;
    if (map_.size() == 0) {
      GM_LOGW(kTag, "no sensors mapped to points: sample not stored");
    } else {
      Sample s;
      memset(&s, 0, sizeof(s));
      s.epoch = m.epoch;
      s.monoS = m.monoS;
      for (int p = 0; p < kMaxPoints; ++p) {
        s.status[p] = static_cast<uint8_t>(ReadStatus::NotMapped);
      }
      for (size_t i = 0; i < m.count; ++i) {
        const SensorReading& r = m.readings[i];
        if (r.point >= 1 && r.point <= kMaxPoints) {
          s.status[r.point - 1] = static_cast<uint8_t>(r.status);
          s.centi[r.point - 1] = r.status == ReadStatus::Ok ? r.centi : 0;
        }
      }
      const uint32_t droppedBefore = st_->droppedOverflow;
      const Sample* stored = st_->push(s);
      if (st_->droppedOverflow != droppedBefore) {
        recordError("buffer full: oldest sample dropped (total %lu)",
                    static_cast<unsigned long>(st_->droppedOverflow));
      }
      GM_LOGI(kTag, "sample #%lu stored, %u buffered",
              static_cast<unsigned long>(stored->seq),
              static_cast<unsigned>(st_->count));
    }
    st_->seal();
  }
  return m;
}

// ---------------------------------------------------------------- upload ----

// Gives every buffered sample a fixed, unique UTC timestamp once the wall
// clock is known. Stored stamps make retries byte-identical, so ThingSpeak
// rejects already stored entries by created_at instead of duplicating them.
void GrainMeter::stampSampleTimes(uint32_t nowEpoch, uint32_t nowMono) {
  for (size_t i = st_->count; i > 0; --i) {
    Sample& s = st_->at(i - 1);
    if (s.epoch == 0) {
      s.epoch = tsSampleEpoch(s, nowEpoch, nowMono);
    }
    if (s.epoch == 0) {
      st_->removeAt(i - 1);  // RTC restarted before time was known
      ++st_->droppedNoTime;
    }
  }
  uint32_t prev = 0;
  for (size_t i = 0; i < st_->count; ++i) {
    Sample& s = st_->at(i);
    if (s.epoch <= prev) {
      s.epoch = prev + 1;
    }
    prev = s.epoch;
  }
  st_->seal();
}

void GrainMeter::finishUpload(bool ok, int status, const char* msg) {
  LastUpload& L = st_->last;
  L.ok = ok ? 1 : 0;
  L.httpStatus = static_cast<int16_t>(status);
  snprintf(L.message, sizeof(L.message), "%s", msg);
  if (ok) {
    GM_LOGI(kTag, "upload: %s", msg);
  } else {
    recordError("upload: %s", msg);
  }
  st_->seal();
}

const LastUpload& GrainMeter::upload() {
  LastUpload& L = st_->last;
  memset(&L, 0, sizeof(L));
  L.attempted = 1;
  L.httpStatus = -1;
  L.monoS = p_.clock->monoS();
  st_->hasUploadAttempt = 1;
  st_->lastUploadAttemptMonoS = L.monoS;
  st_->seal();

  if (!cfg_.uploadConfigured()) {
    finishUpload(false, -1, "reporting not configured (channel / write key)");
    return L;
  }
  char err[64];
  if (!p_.net->connect(cfg_, kWifiConnectTimeoutMs, err, sizeof(err))) {
    ++st_->uploadFailCount;
    char msg[96];
    snprintf(msg, sizeof(msg), "network: %s", err);
    finishUpload(false, -1, msg);
    return L;
  }
  // Sync the clock in every window: the RTC runs from the internal RC
  // oscillator (no 32 kHz crystal on the FireBeetle 2 C6) and drifts. A
  // failed sync is fatal only if the clock was never set.
  if (!p_.net->syncTime(kTimeSyncTimeoutMs) && !p_.clock->wallValid()) {
    p_.http->endSession();
    p_.net->disconnect();
    ++st_->uploadFailCount;
    finishUpload(false, -1, "time sync failed; samples kept");
    return L;
  }
  const uint32_t nowEpoch = p_.clock->epoch();
  const uint32_t nowMono = p_.clock->monoS();
  L.epoch = nowEpoch;
  stampSampleTimes(nowEpoch, nowMono);

  char url[160];
  bool mainOk = true;
  int mainStatus = -1;
  char msg[96] = "no samples to send";
  if (st_->count > 0) {
    BulkInfo info;
    if (!tsBulkUrl(cfg_.reportUrl, cfg_.channelId, url, sizeof(url)) ||
        !tsBuildBulk(*st_, kMaxUploadBatch, cfg_.writeKey, nowEpoch, nowMono,
                     bodyBuf_, sizeof(bodyBuf_), &info)) {
      mainOk = false;
      snprintf(msg, sizeof(msg), "could not build request");
    } else {
      respBuf_[0] = '\0';
      const HttpResult res =
          p_.http->postJson(url, bodyBuf_, strlen(bodyBuf_), kHttpTimeoutMs,
                            respBuf_, sizeof(respBuf_));
      const TsOutcome out =
          tsParseResponse(res.transportOk, res.status, respBuf_);
      mainStatus = res.status;
      L.samplesSent = static_cast<uint16_t>(info.entries);
      if (out == TsOutcome::Confirmed) {
        const size_t removed = st_->removeUpTo(info.lastSeq);
        L.samplesConfirmed = static_cast<uint16_t>(removed);
        ++st_->uploadOkCount;
        snprintf(msg, sizeof(msg), "%u sample(s) confirmed (seq %lu..%lu)",
                 static_cast<unsigned>(removed),
                 static_cast<unsigned long>(info.firstSeq),
                 static_cast<unsigned long>(info.lastSeq));
      } else {
        mainOk = false;
        ++st_->uploadFailCount;
        if (!res.transportOk) {
          snprintf(msg, sizeof(msg), "transport: %s", res.error);
        } else {
          snprintf(msg, sizeof(msg), "HTTP %d, %s; samples kept", res.status,
                   tsOutcomeName(out));
        }
      }
    }
  }

  if (cfg_.diagChannelId != 0 && cfg_.diagWriteKey[0] != '\0' &&
      diagDue(nowMono)) {
    sendDiag(nowEpoch);
  }
  if (cfg_.remoteUpdateEnabled() && updateCheckDue(nowMono)) {
    checkRemoteUpdate();
  }
  p_.http->endSession();
  p_.net->disconnect();
  finishUpload(mainOk, mainStatus, msg);
  return L;
}

bool GrainMeter::diagDue(uint32_t nowMono) const {
  if (cfg_.diagEveryH == 0 || !st_->hasDiag || nowMono < st_->lastDiagMonoS) {
    return true;
  }
  return nowMono - st_->lastDiagMonoS + 60u >= cfg_.diagEveryH * 3600u;
}

bool GrainMeter::updateCheckDue(uint32_t nowMono) const {
  if (!st_->hasUpdateCheck || nowMono < st_->lastUpdateCheckMonoS) {
    return true;
  }
  return nowMono - st_->lastUpdateCheckMonoS + 60u >= cfg_.updateCheckH * 3600u;
}

void GrainMeter::sendDiag(uint32_t nowEpoch) {
  LastUpload& L = st_->last;
  DiagValues d;
  d.batteryMv = p_.sys->batteryMv();
  d.rssi = p_.net->rssi();
  d.sensorsFound = static_cast<uint32_t>(scan_.search.found);
  d.sensorErrors = 0;
  for (size_t i = 0; i < meas_.count; ++i) {
    if (meas_.readings[i].point != 0 &&
        meas_.readings[i].status != ReadStatus::Ok) {
      ++d.sensorErrors;
    }
  }
  d.buffered = st_->count;
  d.dropped = st_->droppedOverflow + st_->droppedNoTime;
  d.uploadFailures = st_->uploadFailCount;
  d.wakeCount = st_->wakeCount;
  char status[96];
  snprintf(status, sizeof(status), "fw=%s wake=%s power=%s", GM_FW_VERSION,
           p_.sys->wakeReason(), powerModeName(powerMode_));
  d.status = status;
  char url[160];
  if (!tsBulkUrl(cfg_.reportUrl, cfg_.diagChannelId, url, sizeof(url)) ||
      !tsBuildDiag(d, cfg_.diagWriteKey, nowEpoch, bodyBuf_,
                   sizeof(bodyBuf_))) {
    return;
  }
  respBuf_[0] = '\0';
  const HttpResult res =
      p_.http->postJson(url, bodyBuf_, strlen(bodyBuf_), kHttpTimeoutMs,
                        respBuf_, sizeof(respBuf_));
  L.diagOk = tsParseResponse(res.transportOk, res.status, respBuf_) ==
                     TsOutcome::Confirmed
                 ? 1
                 : 0;
  if (L.diagOk) {
    st_->hasDiag = 1;
    st_->lastDiagMonoS = p_.clock->monoS();
  } else {
    recordError("diagnostic channel: HTTP %d", res.status);
  }
}

// ---------------------------------------------------------- remote update --

const char* remoteUpdateResultName(RemoteUpdateResult r) {
  switch (r) {
    case RemoteUpdateResult::None:
      return "never_checked";
    case RemoteUpdateResult::UpToDate:
      return "up_to_date";
    case RemoteUpdateResult::Updated:
      return "updated";
    case RemoteUpdateResult::Failed:
      return "failed";
    case RemoteUpdateResult::Skipped:
      return "skipped";
    case RemoteUpdateResult::ServiceRequested:
      return "service_requested";
  }
  return "?";
}

void GrainMeter::setUpdateResult(RemoteUpdateResult r, const char* fmt, ...) {
  st_->updateResult = static_cast<uint8_t>(r);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(st_->updateMessage, sizeof(st_->updateMessage), fmt, ap);
  va_end(ap);
  st_->seal();
  if (r == RemoteUpdateResult::Failed) {
    recordError("remote update: %s", st_->updateMessage);
  } else {
    GM_LOGI(kTag, "remote update: %s", st_->updateMessage);
  }
}

namespace {
// Streams the download into the OTA session while hashing it.
class FirmwareSink : public DownloadSink {
 public:
  FirmwareSink(OtaSession& ota, size_t slotSize) : ota_(ota), slot_(slotSize) {}
  bool begin(size_t contentLength) override {
    return contentLength > 0 ? ota_.start(contentLength, true, "remote")
                             : ota_.start(slot_, false, "remote");
  }
  bool data(const uint8_t* bytes, size_t len) override {
    sha_.update(bytes, len);
    return ota_.feed(bytes, len);
  }
  void digest(uint8_t out[32]) { sha_.finish(out); }

 private:
  OtaSession& ota_;
  size_t slot_;
  Sha256 sha_;
};
}  // namespace

void GrainMeter::checkRemoteUpdate() {
  st_->hasUpdateCheck = 1;
  st_->lastUpdateCheckMonoS = p_.clock->monoS();
  st_->seal();
  if (ota_.busy()) {
    setUpdateResult(RemoteUpdateResult::Skipped, "local upload in progress");
    return;
  }
  bodyBuf_[0] = '\0';
  const HttpResult r =
      p_.http->get(cfg_.updateUrl, kHttpTimeoutMs, bodyBuf_, 2048);
  if (!r.transportOk || r.status != 200) {
    if (!r.transportOk) {
      setUpdateResult(RemoteUpdateResult::Failed, "manifest: %s", r.error);
    } else {
      setUpdateResult(RemoteUpdateResult::Failed, "manifest: HTTP %d",
                      r.status);
    }
    return;
  }
  UpdateManifest m;
  const char* err = nullptr;
  if (!parseUpdateManifest(bodyBuf_, &m, &err)) {
    setUpdateResult(RemoteUpdateResult::Failed, "%s", err);
    return;
  }
  if (m.serviceId != 0) {
    char stored[16] = "";
    p_.store->get("svc_req_id", stored, sizeof(stored));
    char id[16];
    snprintf(id, sizeof(id), "%lu", static_cast<unsigned long>(m.serviceId));
    if (strcmp(stored, id) != 0) {
      p_.store->set("svc_req_id", id);
      p_.store->commit();
      char body[32];
      const int n = snprintf(body, sizeof(body), "minutes=%lu",
                             static_cast<unsigned long>(m.serviceMin));
      char msg[96] = "";
      p_.sys->platformAction("enter_service",
                             Form(body, static_cast<size_t>(n)), msg,
                             sizeof(msg));
      setUpdateResult(RemoteUpdateResult::ServiceRequested,
                      "service portal requested for %lu min (id %s)",
                      static_cast<unsigned long>(m.serviceMin), id);
    }
  }
  if (!m.hasFirmware) {
    if (m.serviceId == 0) {
      setUpdateResult(RemoteUpdateResult::UpToDate, "no firmware in manifest");
    }
    return;
  }
  snprintf(st_->updateLatest, sizeof(st_->updateLatest), "%s", m.version);
  if (compareVersions(m.version, GM_FW_VERSION) <= 0) {
    setUpdateResult(RemoteUpdateResult::UpToDate, "running %s, latest %s",
                    GM_FW_VERSION, m.version);
    return;
  }
  if (strcmp(m.version, p_.sys->rejectedFirmware()) == 0) {
    setUpdateResult(RemoteUpdateResult::Skipped,
                    "%s was rolled back after a failed start; not retried",
                    m.version);
    return;
  }
  if (powerMode_ != PowerMode::Normal) {
    setUpdateResult(RemoteUpdateResult::Skipped, "%s available, battery low",
                    m.version);
    return;
  }
  GM_LOGI(kTag, "downloading firmware %s", m.version);
  FirmwareSink sink(ota_, p_.ota->slotSize());
  const HttpResult d =
      p_.http->download(m.url, kFirmwareDownloadTimeoutMs, sink);
  if (!d.transportOk || d.status != 200 || !ota_.busy()) {
    const char* why = ota_.error()[0] != '\0' ? ota_.error()
                      : d.transportOk         ? "HTTP error"
                                              : d.error;
    ota_.abort("download failed");
    setUpdateResult(RemoteUpdateResult::Failed, "download %s: %s (HTTP %d)",
                    m.version, why, d.status);
    return;
  }
  uint8_t digest[32];
  sink.digest(digest);
  if (memcmp(digest, m.sha256, sizeof(digest)) != 0) {
    ota_.abort("sha256 mismatch");
    setUpdateResult(RemoteUpdateResult::Failed,
                    "sha256 of %s does not match the manifest", m.version);
    return;
  }
  if (strcmp(ota_.image().version, m.version) != 0) {
    ota_.abort("version mismatch");
    setUpdateResult(RemoteUpdateResult::Failed,
                    "image reports version %.32s, manifest %.32s",
                    ota_.image().version, m.version);
    return;
  }
  if (!ota_.finish()) {
    setUpdateResult(RemoteUpdateResult::Failed, "%s", ota_.error());
    return;
  }
  setUpdateResult(RemoteUpdateResult::Updated, "%s installed, restarting",
                  m.version);
  p_.sys->requestRestart();
}

uint32_t GrainMeter::runDue(bool allowUpload) {
  const PowerMode mode = powerModeFor(p_.sys->batteryMv(), cfg_);
  if (mode != powerMode_) {
    GM_LOGW(kTag, "power mode %s -> %s", powerModeName(powerMode_),
            powerModeName(mode));
    powerMode_ = mode;
  }
  Plan plan = schedulePlan(*st_, cfg_, p_.clock->monoS(), powerMode_);
  if (plan.measureDue) {
    measure(true);
    plan = schedulePlan(*st_, cfg_, p_.clock->monoS(), powerMode_);
  }
  if (allowUpload && plan.uploadDue) {
    upload();
  }
  return schedulePlan(*st_, cfg_, p_.clock->monoS(), powerMode_).sleepS;
}

// ---------------------------------------------------------------- config ----

bool GrainMeter::applyConfig(const Form& form, char* err, size_t cap) {
  Config next = cfg_;
  if (!configApplyForm(form, limits_, &next, err, cap)) {
    return false;
  }
  if (!configSave(*p_.store, next)) {
    snprintf(err, cap, "storage write failed; configuration not changed");
    recordError("config: storage write failed");
    return false;  // cfg_ untouched; store may hold a partial write until next
                   // save
  }
  cfg_ = next;
  GM_LOGI(kTag, "configuration saved");
  return true;
}

bool GrainMeter::mapAssign(const char* romHex, long point, long heightCm,
                           char* err, size_t cap) {
  Rom rom;
  if (!romFromHex(romHex, &rom)) {
    snprintf(err, cap, "ROM must be 16 hex characters");
    return false;
  }
  if (heightCm < -1 || heightCm > 32767) {
    snprintf(err, cap, "height_cm out of range");
    return false;
  }
  RomMap next = map_;
  const char* why = nullptr;
  if (point < 1 || point > kMaxPoints ||
      !next.assign(rom, static_cast<uint8_t>(point),
                   static_cast<int16_t>(heightCm), &why)) {
    snprintf(err, cap, "%s", why != nullptr ? why : "point must be 1..8");
    return false;
  }
  if (!romMapSave(*p_.store, next)) {
    snprintf(err, cap, "storage write failed; mapping not changed");
    return false;
  }
  map_ = next;
  GM_LOGI(kTag, "mapped %s -> point %ld (height %ld cm)", romHex, point,
          heightCm);
  return true;
}

bool GrainMeter::mapRemove(const char* romHex, char* err, size_t cap) {
  Rom rom;
  if (!romFromHex(romHex, &rom)) {
    snprintf(err, cap, "ROM must be 16 hex characters");
    return false;
  }
  RomMap next = map_;
  if (!next.remove(rom)) {
    snprintf(err, cap, "ROM not mapped");
    return false;
  }
  if (!romMapSave(*p_.store, next)) {
    snprintf(err, cap, "storage write failed; mapping not changed");
    return false;
  }
  map_ = next;
  return true;
}

size_t GrainMeter::mapAuto() {
  if (!haveScan_) {
    scan();
  }
  RomMap next = map_;
  const size_t added = next.autoAssign(scan_.roms, scan_.search.found);
  if (added > 0) {
    if (!romMapSave(*p_.store, next)) {
      recordError("map: storage write failed");
      return 0;
    }
    map_ = next;
  }
  return added;
}

bool GrainMeter::checkServicePassword(const char* pass) const {
  return pass != nullptr && cfg_.servicePassword[0] != '\0' &&
         secretEquals(pass, cfg_.servicePassword);
}

// ---------------------------------------------------------------- JSON ------

static void writeTemp(JsonWriter& w, const char* key, ReadStatus st,
                      int16_t centi) {
  w.key(key);
  if (st == ReadStatus::Ok) {
    w.centi(centi);
  } else {
    w.null();  // never 0 for a missing value
  }
}

void GrainMeter::writeMeasure(JsonWriter& w) {
  w.key("measurement");
  if (!haveMeasure_) {
    w.null();
    return;
  }
  const MeasureReport& m = meas_;
  w.beginObject()
      .kvu("mono_s", m.monoS)
      .kvu("age_s", p_.clock->monoS() - m.monoS)
      .kvu("epoch", m.epoch)
      .kvb("presence", m.presence)
      .kvu("conversion_ms", m.conversionMs)
      .kvb("conversion_timeout", m.conversionTimeout);
  w.key("points").beginArray();
  for (uint8_t p = 1; p <= kMaxPoints; ++p) {
    const RomMap::Entry* e = map_.findPoint(p);
    w.beginObject().kvu("point", p);
    if (e == nullptr) {
      w.key("rom")
          .null()
          .key("height_cm")
          .null()
          .kv("status", readStatusName(ReadStatus::NotMapped));
      w.key("temp_c").null();
    } else {
      char hex[kRomTextLen + 1];
      w.kv("rom", romToHex(e->rom, hex, sizeof(hex)));
      if (e->heightCm >= 0) {
        w.kv("height_cm", static_cast<int64_t>(e->heightCm));
      } else {
        w.key("height_cm").null();
      }
      ReadStatus st = ReadStatus::Missing;
      int16_t centi = 0;
      for (size_t i = 0; i < m.count; ++i) {
        if (m.readings[i].rom == e->rom) {
          st = m.readings[i].status;
          centi = m.readings[i].centi;
        }
      }
      w.kv("status", readStatusName(st));
      writeTemp(w, "temp_c", st, centi);
    }
    w.endObject();
  }
  w.endArray();
  w.key("unmapped").beginArray();
  for (size_t i = 0; i < m.count; ++i) {
    const SensorReading& r = m.readings[i];
    if (r.point != 0) {
      continue;
    }
    char hex[kRomTextLen + 1];
    char fam[8];
    snprintf(fam, sizeof(fam), "0x%02X", r.rom.family());
    w.beginObject()
        .kv("rom", romToHex(r.rom, hex, sizeof(hex)))
        .kv("family", fam);
    w.kv("family_name", familyName(r.rom.family()))
        .kv("status", readStatusName(r.status));
    writeTemp(w, "temp_c", r.status, r.centi);
    w.endObject();
  }
  w.endArray().endObject();
}

void GrainMeter::writeScan(JsonWriter& w) {
  w.key("scan");
  if (!haveScan_) {
    w.null();
    return;
  }
  const SearchResult& r = scan_.search;
  w.beginObject()
      .kvu("age_s", p_.clock->monoS() - scan_.monoS)
      .kvb("presence", r.presence)
      .kvb("bus_error", r.busError)
      .kvb("truncated", r.truncated)
      .kvu("rom_crc_errors", r.crcErrors);
  w.key("devices").beginArray();
  for (size_t i = 0; i < r.found; ++i) {
    const Rom& rom = scan_.roms[i];
    char hex[kRomTextLen + 1];
    char fam[8];
    snprintf(fam, sizeof(fam), "0x%02X", rom.family());
    w.beginObject()
        .kv("rom", romToHex(rom, hex, sizeof(hex)))
        .kv("family", fam)
        .kv("family_name", familyName(rom.family()))
        .kvb("supported", familySupported(rom.family()))
        .kvu("point", map_.pointOf(rom))
        .endObject();
  }
  w.endArray().endObject();
}

void GrainMeter::writeBuffer(JsonWriter& w) {
  const Sample* oldest = st_->count > 0 ? &st_->at(0) : nullptr;
  w.key("buffer")
      .beginObject()
      .kvu("count", st_->count)
      .kvu("capacity", kBufferCapacity)
      .kvu("next_seq", st_->nextSeq)
      .kvu("oldest_seq", oldest != nullptr ? oldest->seq : 0)
      .kvu("dropped_overflow", st_->droppedOverflow)
      .kvu("dropped_no_time", st_->droppedNoTime)
      .kvu("upload_ok", st_->uploadOkCount)
      .kvu("upload_fail", st_->uploadFailCount)
      .kvu("wake_count", st_->wakeCount)
      .kvb("recovered_after_power_loss", stateRecovered_)
      .endObject();
}

void GrainMeter::writeLastUpload(JsonWriter& w) {
  const LastUpload& L = st_->last;
  w.key("last_upload");
  if (!L.attempted) {
    w.null();
    return;
  }
  w.beginObject()
      .kvb("ok", L.ok != 0)
      .kvb("diag_ok", L.diagOk != 0)
      .kv("http_status", static_cast<int64_t>(L.httpStatus))
      .kvu("epoch", L.epoch)
      .kvu("age_s", p_.clock->monoS() - L.monoS)
      .kvu("samples_sent", L.samplesSent)
      .kvu("samples_confirmed", L.samplesConfirmed)
      .kv("message", L.message)
      .endObject();
}

void GrainMeter::writePower(JsonWriter& w) {
  const int mv = p_.sys->batteryMv();
  w.key("power").beginObject().kv("mode", powerModeName(powerMode_));
  if (mv >= 0) {
    w.kv("battery_mv", static_cast<int64_t>(mv));
  } else {
    w.key("battery_mv").null();
  }
  w.kvu("low_mv", cfg_.battLowMv).kvu("crit_mv", cfg_.battCritMv).endObject();
}

void GrainMeter::writeRemoteUpdate(JsonWriter& w) {
  const RemoteUpdateResult r =
      static_cast<RemoteUpdateResult>(st_->updateResult);
  w.key("remote_update")
      .beginObject()
      .kvb("enabled", cfg_.remoteUpdateEnabled())
      .kv("running", GM_FW_VERSION)
      .kv("latest", st_->updateLatest)
      .kv("result", remoteUpdateResultName(r))
      .kv("message", st_->updateMessage)
      .kv("rejected", p_.sys->rejectedFirmware());
  w.key("last_check_age_s");
  if (st_->hasUpdateCheck) {
    w.unum(p_.clock->monoS() - st_->lastUpdateCheckMonoS);
  } else {
    w.null();
  }
  w.endObject();
}

void GrainMeter::writeErrors(JsonWriter& w) {
  w.key("errors").beginArray();
  const size_t start = (errorNext_ + kErrorSlots - errorCount_) % kErrorSlots;
  for (size_t i = 0; i < errorCount_; ++i) {
    const ErrorEntry& e = errors_[(start + i) % kErrorSlots];
    w.beginObject()
        .kvu("age_s", p_.clock->monoS() - e.monoS)
        .kv("text", e.text)
        .endObject();
  }
  w.endArray();
}

void GrainMeter::writeStatus(JsonWriter& w) {
  const uint32_t now = p_.clock->monoS();
  const Plan plan = schedulePlan(*st_, cfg_, now, powerMode_);
  w.beginObject()
      .kv("firmware", GM_FW_VERSION)
      .kv("platform", p_.sys->platformName())
      .kv("name", cfg_.deviceName)
      .kvu("mono_s", now)
      .kvb("time_valid", p_.clock->wallValid())
      .kvu("epoch", p_.clock->wallValid() ? p_.clock->epoch() : 0)
      .kv("wake_reason", p_.sys->wakeReason())
      .kv("reset_reason", p_.sys->resetReason());
  const int mv = p_.sys->batteryMv();
  if (mv >= 0) {
    w.kv("battery_mv", static_cast<int64_t>(mv));
  } else {
    w.key("battery_mv").null();
  }
  w.key("wifi")
      .beginObject()
      .kv("state", p_.net->stateText())
      .kv("ssid", cfg_.wifiSsid)
      .kv("rssi", static_cast<int64_t>(p_.net->rssi()))
      .endObject();
  w.kvb("config_ok", configOk_)
      .kvb("upload_configured", cfg_.uploadConfigured());
  w.key("schedule")
      .beginObject()
      .kvu("measure_s", cfg_.measureIntervalS)
      .kvu("upload_s", cfg_.uploadIntervalS)
      .kvu("samples_per_upload", cfg_.samplesPerUpload())
      .kvu("next_measure_in_s", plan.measureDue ? 0 : plan.sleepS)
      .kvb("upload_due", plan.uploadDue)
      .endObject();
  writeMeasure(w);
  writeBuffer(w);
  writeLastUpload(w);
  writePower(w);
  writeRemoteUpdate(w);
  writeErrors(w);
  w.key("ota");
  ota_.writeJson(w);
  p_.sys->writePlatformJson(w, false);
  w.endObject();
}

void GrainMeter::writeDiag(JsonWriter& w) {
  w.beginObject()
      .kv("firmware", GM_FW_VERSION)
      .kv("platform", p_.sys->platformName());
  w.kvu("mono_s", p_.clock->monoS()).kvb("time_valid", p_.clock->wallValid());
  w.kv("wake_reason", p_.sys->wakeReason())
      .kv("reset_reason", p_.sys->resetReason());
  w.kv("battery_mv", static_cast<int64_t>(p_.sys->batteryMv()));
  w.key("wifi")
      .beginObject()
      .kv("state", p_.net->stateText())
      .kv("rssi", static_cast<int64_t>(p_.net->rssi()));
  w.endObject();
  w.key("config");
  configWriteJson(w, cfg_);
  w.key("rom_map").beginArray();
  for (size_t i = 0; i < map_.size(); ++i) {
    char hex[kRomTextLen + 1];
    w.beginObject()
        .kv("rom", romToHex(map_.at(i).rom, hex, sizeof(hex)))
        .kvu("point", map_.at(i).point)
        .kv("height_cm", static_cast<int64_t>(map_.at(i).heightCm))
        .endObject();
  }
  w.endArray();
  writeScan(w);
  writeMeasure(w);
  writeBuffer(w);
  writeLastUpload(w);
  writePower(w);
  writeRemoteUpdate(w);
  writeErrors(w);
  w.key("ota");
  ota_.writeJson(w);
  p_.sys->writePlatformJson(w, true);
  w.endObject();
}

}  // namespace gm
