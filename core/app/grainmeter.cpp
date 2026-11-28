// SPDX-License-Identifier: MIT
#include "app/grainmeter.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app/alarm.h"
#include "app/battery_forecast.h"
#include "app/notifications.h"
#include "comm/talkback.h"
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
      otaUpload_(ota_, *ports.clock),
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
  p_.sys->configApplied(cfg_);
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
  if (p_.clock->wallRestored()) {
    // Persist the retry requirement even if we sleep before the next window.
    st_->hasTimeSync = 0;
    st_->timeSource = 0;
  }
  if (static_cast<RemoteUpdateResult>(st_->updateResult) ==
          RemoteUpdateResult::Available &&
      compareVersions(st_->updateLatest, GM_FW_VERSION) <= 0) {
    // installed meanwhile (e.g. from a file): nothing newer to offer
    setUpdateResult(RemoteUpdateResult::UpToDate, "running %s", GM_FW_VERSION);
  }
  st_->seal();
  if (p_.history != nullptr && !history_.begin(p_.history)) {
    recordError("history storage unusable; local history disabled");
  }
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

size_t GrainMeter::assignPointHints(RomMap& next, const PointHint* hints,
                                    size_t n,
                                    uint8_t conflictPoints[kMaxPoints],
                                    size_t* conflicts) {
  const size_t added =
      next.assignByHints(hints, n, scan_.roms, scan_.search.found,
                         conflictPoints, kMaxPoints, conflicts);
  for (size_t i = 0; i < *conflicts; ++i) {
    recordError("map: TH/TL point %u claimed by several sensors",
                conflictPoints[i]);
  }
  return added;
}

bool GrainMeter::saveHintMap(const RomMap& next, const PointHint* hints,
                             size_t n) {
  if (!romMapSave(*p_.store, next)) {
    recordError("map: storage write failed");
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (map_.pointOf(hints[i].rom) == 0 &&
        next.pointOf(hints[i].rom) == hints[i].point) {
      char hex[kRomTextLen + 1];
      GM_LOGI(kTag, "map: ROM %s -> point %u (TH/TL)",
              romToHex(hints[i].rom, hex, sizeof(hex)), hints[i].point);
    }
  }
  map_ = next;
  return true;
}

const MeasureReport& GrainMeter::measure(bool store) {
  p_.clock->checkpoint();
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
  PointHint hints[kMaxSensors];
  size_t nHints = 0;
  for (size_t i = 0; i < m.count; ++i) {
    SensorReading& r = m.readings[i];
    if (!familySupported(r.rom.family())) {
      r.status = ReadStatus::Unsupported;
    } else if (!m.presence) {
      r.status = ReadStatus::Missing;
    } else {
      uint8_t raw[kScratchpadLen] = {};
      r.status =
          readSensor(*p_.bus, r.rom, &r.centi, r.point == 0 ? raw : nullptr);
      if (r.point == 0) {
        const uint8_t point = pointFromScratchpad(raw);
        if (point != 0) {
          hints[nHints++] = {r.rom, point};
        }
      }
    }
  }
  RomMap next = map_;
  uint8_t conflictPoints[kMaxPoints];
  size_t conflicts = 0;
  if (assignPointHints(next, hints, nHints, conflictPoints, &conflicts) > 0 &&
      saveHintMap(next, hints, nHints)) {
    for (size_t i = 0; i < m.count; ++i) {
      m.readings[i].point = map_.pointOf(m.readings[i].rom);
    }
  }
  for (size_t i = 0; i < m.count; ++i) {
    const SensorReading& r = m.readings[i];
    if (r.point != 0 && r.status != ReadStatus::Ok) {
      char hex[kRomTextLen + 1];
      recordError("point %u (%s): %s", r.point,
                  romToHex(r.rom, hex, sizeof(hex)), readStatusName(r.status));
    }
  }
  haveMeasure_ = true;

  if (store) {
    // Previous measurement (for the local history's plateau end); none
    // after a power-up -> the history marks a restart.
    const bool hadPrev = st_->hasMeasured != 0;
    const uint32_t prevMono = st_->lastMeasureMonoS;
    st_->hasMeasured = 1;
    st_->lastMeasureMonoS = m.monoS;
    // Battery at every measurement, also without sensors: diagnostic channel
    // field1 in bulk (statistics on ThingSpeak).
    const int mv = p_.sys->batteryMv();
    if (mv > 0) {
      BatteryPoint bp;
      memset(&bp, 0, sizeof(bp));
      bp.epoch = m.epoch;
      bp.monoS = m.monoS;
      bp.mv = static_cast<uint16_t>(mv > 65535 ? 65535 : mv);
      st_->battPush(bp);
      checkCharging(mv, m.monoS);
    }
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
      // Local history (portal charts without Internet) needs a wall clock.
      if (s.epoch != 0 && history_.available()) {
        HistoryRecord hr = historyRecord(s, false);
        uint32_t prevEpoch = 0;
        if (!hadPrev) {
          hr.flags |= kHistRestart;
        } else if (m.monoS > prevMono && m.monoS - prevMono < s.epoch) {
          prevEpoch = s.epoch - (m.monoS - prevMono);
        }
        if (!history_.append(hr, prevEpoch)) {
          recordError("history: flash write failed");
        }
      }
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
  alarmEvaluate(st_->alarm, cfg_, m, history_, historyMaxGapS());
  st_->seal();
  return m;
}

// ---------------------------------------------------------------- upload ----

// Gives every buffered sample a fixed, unique UTC timestamp once the wall
// clock is known. Stored stamps make retries byte-identical, so ThingSpeak
// rejects already stored entries by created_at instead of duplicating them.
HistoryRecord GrainMeter::historyRecord(const Sample& s, bool reconstructed) {
  HistoryRecord r;
  memset(&r, 0, sizeof(r));
  r.epoch = s.epoch;
  memcpy(r.centi, s.centi, sizeof(r.centi));
  memcpy(r.status, s.status, sizeof(r.status));
  for (int i = 0; i < kMaxPoints; ++i) {
    if (r.status[i] != static_cast<uint8_t>(ReadStatus::Ok)) {
      r.centi[i] = kHistoryMissing;
    }
  }
  if (reconstructed) {
    // device state at capture time is unknown for backfilled samples
    r.flags = 3 | kHistReconstructed;
    return r;
  }
  const int mv = p_.sys->batteryMv();
  r.batteryMv = mv > 0 && mv < 65535 ? static_cast<uint16_t>(mv) : 0;
  r.rssi = st_->lastRssi;
  const LastUpload& L = st_->last;
  const uint8_t up = !L.attempted ? 0 : (L.ok ? 1 : 2);
  r.flags = static_cast<uint8_t>(static_cast<uint8_t>(powerMode_) |
                                 (up << kHistUploadShift));
  return r;
}

void GrainMeter::stampSampleTimes(uint32_t nowEpoch, uint32_t nowMono) {
  // Samples measured before the clock was known were not written to the
  // local history; they are added once their time is reconstructed.
  uint32_t fresh[kBufferCapacity];
  size_t freshCount = 0;
  for (size_t i = st_->count; i > 0; --i) {
    Sample& s = st_->at(i - 1);
    if (s.epoch == 0) {
      s.epoch = tsSampleEpoch(s, nowEpoch, nowMono);
      if (s.epoch != 0 && freshCount < kBufferCapacity) {
        fresh[freshCount++] = s.seq;
      }
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
  if (freshCount == 0 || !history_.available()) {
    return;
  }
  // Samples without a clock exist only after a power-up: the first one
  // starts a new line in the history.
  uint32_t prevEpoch = 0;
  bool failed = false;
  for (size_t i = 0; i < st_->count; ++i) {  // oldest first
    const Sample& s = st_->at(i);
    for (size_t k = 0; k < freshCount; ++k) {
      if (fresh[k] == s.seq) {
        HistoryRecord hr = historyRecord(s, true);
        if (prevEpoch == 0) hr.flags |= kHistRestart;
        if (!history_.append(hr, prevEpoch) && !failed) {
          failed = true;
          recordError("history: flash write failed");
        }
        prevEpoch = s.epoch;
        break;
      }
    }
  }
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

const LastUpload& GrainMeter::upload(bool alarmWindow) {
  const uint8_t prevAttempt = st_->hasUploadAttempt;
  const uint32_t prevMono = st_->lastUploadAttemptMonoS;
  if (alarmWindow) {
    AlarmState& a = st_->alarm;
    if (!a.pendingWindow && a.active) {
      NotifyEvent e = {
          p_.clock->epoch(), p_.clock->monoS(), a.centi, a.riseCenti,
          NotifyAlarm,       a.reason,          a.point, 0};
      alarmEnqueue(a, e);
    }
    a.pendingWindow = 0;
    a.hasWindow = 1;
    a.lastWindowMonoS = p_.clock->monoS();
  }
  if (!alarmWindow) st_->alarm.samplesSentSinceRegular = 0;
  LastUpload& L = st_->last;
  memset(&L, 0, sizeof(L));
  L.attempted = 1;
  L.httpStatus = -1;
  L.monoS = p_.clock->monoS();
  st_->hasUploadAttempt = 1;
  st_->lastUploadAttemptMonoS = L.monoS;
  if (alarmWindow) {
    st_->hasUploadAttempt = prevAttempt;
    st_->lastUploadAttemptMonoS = prevMono;
  }
  st_->seal();

  if (!cfg_.uploadConfigured() && !notificationsConfigured(cfg_) &&
      !cfg_.talkbackId) {
    finishUpload(false, -1, "reporting not configured (channel / write key)");
    return L;
  }
  char err[64];
  const bool connected =
      p_.net->connect(cfg_, kWifiConnectTimeoutMs, err, sizeof(err));
  if (connected) {
    const int rssi = p_.net->rssi();  // radio is on only in upload windows
    st_->lastRssi = static_cast<int8_t>(rssi < -127 ? -127
                                        : rssi > 0  ? 0
                                                    : rssi);
  }
  if (!connected) {
    ++st_->uploadFailCount;
    char msg[96];
    snprintf(msg, sizeof(msg), "network: %s", err);
    finishUpload(false, -1, msg);
    return L;
  }
  // The RTC runs from the internal RC oscillator (no 32 kHz crystal on the
  // FireBeetle 2 C6) and drifts; the HTTP Date of every ThingSpeak reply
  // corrects it (adjustClock), SNTP runs only when the clock is unknown or
  // no sync succeeded for a day. A failed sync is fatal only if the clock
  // was never set.
  const bool timeOk = synchronizeTime();
  if (!timeOk && !p_.clock->wallValid()) {
    pollTalkback();
    p_.http->endSession();
    p_.net->disconnect();
    // A heartbeat window without samples has nothing to fail with.
    if (st_->count > 0) ++st_->uploadFailCount;
    finishUpload(false, -1,
                 st_->count > 0 ? "time sync failed; samples kept"
                                : "time sync failed (no samples)");
    return L;
  }
  // TalkBack first: a queued REMOTE should start the session as early as
  // possible (the session itself starts after this window).
  // TLS certificate validity checks need the synchronised wall clock.
  pollTalkback();
  sendNotifications();
  const uint32_t nowEpoch = p_.clock->epoch();
  const uint32_t nowMono = p_.clock->monoS();
  L.epoch = nowEpoch;
  stampSampleTimes(nowEpoch, nowMono);

  char url[160];
  bool mainOk = true;
  int mainStatus = -1;
  char msg[96] = "no samples to send";
  if (st_->count > 0 && cfg_.uploadConfigured()) {
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
      adjustClock(res);
      const TsOutcome out =
          tsParseResponse(res.transportOk, res.status, respBuf_);
      mainStatus = res.status;
      L.samplesSent = static_cast<uint16_t>(info.entries);
      if (out == TsOutcome::Confirmed) {
        const size_t removed = st_->removeUpTo(info.lastSeq);
        L.samplesConfirmed = static_cast<uint16_t>(removed);
        if (alarmWindow) {
          const size_t total = st_->alarm.samplesSentSinceRegular + removed;
          st_->alarm.samplesSentSinceRegular = static_cast<uint16_t>(
              total > kBufferCapacity ? static_cast<size_t>(kBufferCapacity)
                                      : total);
        }
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

  if (cfg_.diagChannelId == 0 || cfg_.diagWriteKey[0] == '\0') {
    L.diagResult = DiagNotConfigured;
  } else if (st_->battCount > 0 || diagDue(nowMono)) {
    sendDiag(nowEpoch, nowMono, diagDue(nowMono));
  } else {
    L.diagResult = DiagNotDue;
  }
  // With a remote session waiting, the manifest check (a second TLS
  // handshake, ~6 s) waits for the next window: the user is waiting.
  if (cfg_.remoteUpdateEnabled() && updateCheckDue(nowMono) &&
      remotePendingMin() == 0) {
    // Scheduled checks only inform: installing is always the user's click
    // (portal: Update -> Install). A newly announced version is reported
    // once through ntfy/Telegram when configured.
    char before[sizeof(st_->updateLatest)];
    memcpy(before, st_->updateLatest, sizeof(before));
    checkRemoteUpdate(false, false, "");
    if (lastRemoteUpdateResult() == RemoteUpdateResult::Available &&
        strcmp(before, st_->updateLatest) != 0 &&
        notificationsConfigured(cfg_)) {
      char text[160];
      snprintf(text, sizeof(text),
               "%s: dostępna nowa wersja oprogramowania %s (działa %s). "
               "Instalacja: portal -> Aktualizacja.",
               cfg_.deviceName, st_->updateLatest, GM_FW_VERSION);
      for (unsigned ch = 0; ch < 2; ++ch) notifySend(cfg_, *p_.http, ch, text);
    }
  }
  p_.http->endSession();
  p_.net->disconnect();
  finishUpload(mainOk, mainStatus, msg);
  return L;
}

void GrainMeter::refreshRemoteBudget() {
  const bool utc = p_.clock->wallValid();
  const uint32_t day = utc ? p_.clock->epoch() / 86400 : 0;
  const uint32_t mono = p_.clock->monoS();
  if (!st_->remoteBudgetStarted) return;
  if (!utc && st_->remoteBudgetUtc) {
    // Clock validity was lost: retain usage and start a conservative rolling
    // window rather than granting another daily allowance immediately.
    st_->remoteBudgetUtc = 0;
    st_->remoteWindowMonoS = mono;
  } else if (utc && !st_->remoteBudgetUtc) {
    // Preserve usage when time first becomes known; next UTC midnight resets.
    st_->remoteBudgetUtc = 1;
    st_->remoteDay = day;
  } else if ((utc && st_->remoteBudgetUtc && day > st_->remoteDay) ||
             (!st_->remoteBudgetUtc &&
              mono - st_->remoteWindowMonoS >= 86400)) {
    st_->remoteUsedS = 0;
    st_->remoteDay = day;
    st_->remoteWindowMonoS = mono;
  }
  st_->seal();
}

uint32_t GrainMeter::remoteLeftS() {
  refreshRemoteBudget();
  const uint32_t max = cfg_.remoteBudgetMin * 60;
  return st_->remoteUsedS >= max ? 0 : max - st_->remoteUsedS;
}

uint32_t GrainMeter::remotePendingMin() {
  RemoteCommand c = {pendingRemoteMin_, pendingRemoteUntil_};
  if (talkbackExpired(c, p_.clock->wallValid(), p_.clock->epoch()))
    pendingRemoteMin_ = 0;
  if (powerModeFor(p_.sys->batteryMv(), cfg_) == PowerMode::Critical) return 0;
  const uint32_t left = remoteLeftS() / 60;
  return pendingRemoteMin_ < left ? pendingRemoteMin_ : left;
}

bool GrainMeter::takeRemoteRequest(uint32_t* minutes) {
  if (!minutes) return false;
  *minutes = remotePendingMin();
  pendingRemoteMin_ = 0;
  return *minutes != 0;
}

void GrainMeter::remoteSessionUsed(uint32_t seconds) {
  if (!seconds) return;
  refreshRemoteBudget();
  if (!st_->remoteBudgetStarted) {
    st_->remoteBudgetStarted = 1;
    st_->remoteBudgetUtc = p_.clock->wallValid();
    st_->remoteDay = st_->remoteBudgetUtc ? p_.clock->epoch() / 86400 : 0;
    st_->remoteWindowMonoS = p_.clock->monoS();
  }
  // Saturate without losing accounting if the configured budget changes.
  st_->remoteUsedS = seconds > UINT32_MAX - st_->remoteUsedS
                         ? UINT32_MAX
                         : st_->remoteUsedS + seconds;
  st_->seal();
}

void GrainMeter::pollTalkback() {
  if (!cfg_.talkbackId ||
      powerModeFor(p_.sys->batteryMv(), cfg_) == PowerMode::Critical)
    return;
  char command[128];
  const char* base =
      limits_.allowInsecureUrl && strncmp(cfg_.reportUrl, "mock://", 7) == 0
          ? "mock://talkback"
          : "https://api.thingspeak.com";
  int status = -1;
  const TalkbackResult r =
      talkbackExecute(*p_.http, base, cfg_.talkbackId, cfg_.talkbackKey,
                      command, sizeof(command), &status);
  if (r == TalkbackResult::Failed) {
    recordError("TalkBack request failed (%s%d)",
                status < 0      ? "no connection "
                : status != 200 ? "HTTP "
                                : "bad response ",
                status);
    return;
  }
  if (r == TalkbackResult::Empty) return;
  RemoteCommand c = {};
  const bool valid = talkbackParseCommand(command, &c);
  // A queued command is untrusted and may echo a credential: redact before
  // keeping any diagnostic text, including malformed/unknown commands.
  const char* secrets[] = {cfg_.talkbackKey, cfg_.remotePass, cfg_.writeKey,
                           cfg_.servicePassword};
  for (const char* secret : secrets) {
    if (!*secret) continue;
    char* search = command;
    char* at;
    while ((at = strstr(search, secret)) != nullptr) {
      memset(at, '*', strlen(secret));
      search = at + strlen(secret);
    }
  }
  for (char* p = command; *p; ++p)
    if (static_cast<unsigned char>(*p) < 32) *p = ' ';
  snprintf(lastRemoteCmd_, sizeof(lastRemoteCmd_), "%s", command);
  lastRemoteEpoch_ = p_.clock->wallValid() ? p_.clock->epoch() : 0;
  lastRemoteMonoS_ = p_.clock->monoS();
  if (!valid) {
    recordError("TalkBack unknown: %.58s", command);
    return;
  }
  if (talkbackExpired(c, p_.clock->wallValid(), p_.clock->epoch())) return;
  pendingRemoteMin_ = c.minutes;
  pendingRemoteUntil_ = c.validUntil;
}

void GrainMeter::writeRemote(JsonWriter& w) {
  const uint32_t pending = remotePendingMin();
  RemoteSessionInfo info;
  if (p_.remote)
    info = p_.remote->info();
  else
    info.budgetLeftS = remoteLeftS();
  w.key("remote")
      .beginObject()
      .kvb("configured", cfg_.remoteUrl[0] != 0)
      .kvb("talkback", cfg_.talkbackId != 0)
      .kvu("budget_min", cfg_.remoteBudgetMin)
      .kvu("budget_left_min", info.budgetLeftS / 60)
      .kvu("budget_left_s", info.budgetLeftS)
      .kvu("remaining_s", info.remainingS)
      .kvu("until", info.remainingS && p_.clock->wallValid()
                        ? p_.clock->epoch() + info.remainingS
                        : 0)
      .key("last_cmd")
      .beginObject()
      .kv("text", lastRemoteCmd_)
      .kvu("epoch", lastRemoteEpoch_)
      .kvu("mono_s", lastRemoteMonoS_)
      .endObject()
      .kvu("pending_min", pending)
      .endObject();
}

bool GrainMeter::diagDue(uint32_t nowMono) const {
  if (cfg_.diagEveryH == 0 || !st_->hasDiag || nowMono < st_->lastDiagMonoS) {
    return true;
  }
  return nowMono - st_->lastDiagMonoS + 60u >= cfg_.diagEveryH * 3600u;
}

void GrainMeter::checkCharging(int mv, uint32_t monoS) {
  int16_t t = 0;
  if (p_.sys->boardTempCenti(&t)) {
    st_->boardTempCenti = t;
    st_->hasBoardTemp = 1;
  }
  if (st_->prevBattMv != 0 && monoS > st_->prevBattMonoS) {
    const int dv = mv - static_cast<int>(st_->prevBattMv);
    const uint32_t dt = monoS - st_->prevBattMonoS;
    if (dv >= kChargeRiseMinMv &&
        static_cast<int64_t>(dv) * 3600 >=
            static_cast<int64_t>(kChargeRiseMvPerH) * dt) {
      st_->charging = 1;
    } else if (dv <= -kChargeRiseMinMv) {
      st_->charging = 0;  // falling again: discharging
    }  // flat: keep (constant-voltage phase near 4.2 V)
  }
  st_->prevBattMv = static_cast<uint16_t>(mv);
  st_->prevBattMonoS = monoS;
  const bool cold = st_->hasBoardTemp && st_->boardTempCenti <= kFrostOnCenti;
  const bool warm = !st_->hasBoardTemp || st_->boardTempCenti >= kFrostOffCenti;
  if (st_->charging && cold && !st_->frostCharging) {
    st_->frostCharging = 1;
    recordError(
        "charging at %d.%d C: LiPo must not be charged below 0 C",
        st_->boardTempCenti / 100,
        (st_->boardTempCenti < 0 ? -st_->boardTempCenti : st_->boardTempCenti) /
            10 % 10);
    NotifyEvent e;
    memset(&e, 0, sizeof(e));
    e.epoch = p_.clock->wallValid() ? p_.clock->epoch() : 0;
    e.monoS = monoS;
    e.centi = st_->boardTempCenti;
    e.type = NotifyFrostCharge;
    alarmEnqueue(st_->alarm, e);
  } else if (st_->frostCharging && (!st_->charging || warm)) {
    st_->frostCharging = 0;
  }
}

bool GrainMeter::timeSyncDue(uint32_t nowMono) const {
  return !p_.clock->wallValid() || !st_->hasTimeSync ||
         nowMono < st_->lastTimeSyncMonoS ||
         nowMono - st_->lastTimeSyncMonoS >= kTimeSyncEveryS;
}

bool GrainMeter::synchronizeTime() {
  if (!timeSyncDue(p_.clock->monoS())) return true;
  if (p_.net->syncTime(kTimeSyncTimeoutMs) && p_.clock->wallValid()) {
    noteTimeSync(1);
    return true;
  }
  recordError("time sync failed%s", p_.clock->wallValid()
                                        ? "; using retained clock"
                                        : "; clock unknown");
  return false;
}

void GrainMeter::noteTimeSync(uint8_t source) {
  p_.clock->checkpoint();
  st_->hasTimeSync = 1;
  st_->timeSource = source;
  st_->lastTimeSyncMonoS = p_.clock->monoS();
  st_->seal();
}

void GrainMeter::adjustClock(const HttpResult& r) {
  if (!r.transportOk || r.serverEpoch == 0) {
    return;
  }
  if (p_.clock->wallValid()) {
    const int64_t diff = static_cast<int64_t>(r.serverEpoch) -
                         static_cast<int64_t>(p_.clock->epoch());
    if (diff >= kClockFixMinS || diff <= -kClockFixMinS) {
      if (!p_.clock->setEpoch(r.serverEpoch)) {
        return;
      }
      st_->lastClockFixS = static_cast<int16_t>(diff > 32767    ? 32767
                                                : diff < -32767 ? -32767
                                                                : diff);
      GM_LOGI(kTag, "clock corrected by %+ld s (HTTP Date)",
              static_cast<long>(diff));
    }
  } else if (!p_.clock->setEpoch(r.serverEpoch)) {
    return;
  }
  noteTimeSync(2);
}

void GrainMeter::writeClock(JsonWriter& w) {
  const uint32_t now = p_.clock->monoS();
  w.key("clock").beginObject().kv("source", !st_->hasTimeSync      ? "none"
                                            : st_->timeSource == 2 ? "http_date"
                                                                   : "sntp");
  if (st_->hasTimeSync && now >= st_->lastTimeSyncMonoS) {
    w.kvu("sync_age_s", now - st_->lastTimeSyncMonoS);
  } else {
    w.key("sync_age_s").null();
  }
  w.kv("last_fix_s", static_cast<int64_t>(st_->lastClockFixS))
      .kvb("restored", p_.clock->wallRestored())
      .endObject();
}

bool GrainMeter::updateCheckDue(uint32_t nowMono) const {
  if (!st_->hasUpdateCheck || nowMono < st_->lastUpdateCheckMonoS) {
    return true;
  }
  return nowMono - st_->lastUpdateCheckMonoS + 60u >= cfg_.updateCheckH * 3600u;
}

void GrainMeter::sendDiag(uint32_t nowEpoch, uint32_t nowMono, bool full) {
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
  char status[256];
  const int32_t mah = p_.sys->batteryMahPerDayCenti();
  if (!tsFormatDiagStatus(
          GM_FW_VERSION, p_.sys->wakeReason(), powerModeName(powerMode_),
          cfg_.battCapacityMah, mah,
          batteryRuntimeCentiDays(d.batteryMv, cfg_.battCapacityMah, mah),
          cfg_.uploadIntervalS, cfg_.measureIntervalS, cfg_.lanWake != 0,
          cfg_.talkbackId != 0, status, sizeof(status))) {
    L.diagResult = DiagFailed;
    recordError("diagnostic channel: status too large");
    return;
  }
  d.status = status;
  char url[160];
  size_t points = 0;
  if (!tsBulkUrl(cfg_.reportUrl, cfg_.diagChannelId, url, sizeof(url)) ||
      !tsBuildDiagBulk(*st_, full ? &d : nullptr, cfg_.diagWriteKey, nowEpoch,
                       nowMono, bodyBuf_, sizeof(bodyBuf_), &points)) {
    L.diagResult = DiagFailed;
    recordError("diagnostic channel: request too large");
    return;
  }
  respBuf_[0] = '\0';
  const HttpResult res =
      p_.http->postJson(url, bodyBuf_, strlen(bodyBuf_), kHttpTimeoutMs,
                        respBuf_, sizeof(respBuf_));
  adjustClock(res);
  L.diagOk = tsParseResponse(res.transportOk, res.status, respBuf_) ==
                     TsOutcome::Confirmed
                 ? 1
                 : 0;
  L.diagResult = L.diagOk ? DiagSent : DiagFailed;
  if (L.diagOk) {
    st_->battRemoveFront(points);
    if (full) {
      st_->hasDiag = 1;
      st_->lastDiagMonoS = p_.clock->monoS();
    }
    st_->seal();
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
    case RemoteUpdateResult::Available:
      return "available";
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
  if (r == RemoteUpdateResult::Failed && !manualRun_) {
    recordError("remote update: %s", st_->updateMessage);
  } else {
    GM_LOGI(kTag, "remote update: %s", st_->updateMessage);
  }
}

namespace {
// Streams the download into the OTA session while hashing it.
class FirmwareSink : public DownloadSink {
 public:
  FirmwareSink(OtaSession& ota, size_t slotSize, Clock& clock,
               uint32_t deadlineMonoS)
      : ota_(ota), slot_(slotSize), clock_(clock), deadline_(deadlineMonoS) {}
  bool timedOut() const { return timedOut_; }
  bool begin(size_t contentLength) override {
    return contentLength > 0 ? ota_.start(contentLength, true, "remote")
                             : ota_.start(slot_, false, "remote");
  }
  bool data(const uint8_t* bytes, size_t len) override {
    if (clock_.monoS() > deadline_) {  // slow link: give up, retry later
      timedOut_ = true;
      return false;
    }
    sha_.update(bytes, len);
    return ota_.feed(bytes, len);
  }
  void digest(uint8_t out[32]) { sha_.finish(out); }

 private:
  OtaSession& ota_;
  size_t slot_;
  Clock& clock_;
  uint32_t deadline_;
  bool timedOut_ = false;
  Sha256 sha_;
};
}  // namespace

RemoteUpdateResult GrainMeter::refuseManual(RemoteUpdateResult r,
                                            const char* msg) {
  snprintf(manualMsg_, sizeof(manualMsg_), "%s", msg);
  GM_LOGI(kTag, "remote update (portal): %s", manualMsg_);
  return r;
}

RemoteUpdateResult GrainMeter::remoteUpdateNow(bool install,
                                               const char* expect) {
  // Refusals before the manifest is read are only answered to the portal;
  // the stored result of the last real check stays.
  manualMsg_[0] = '\0';
  if (cfg_.updateUrl[0] == '\0') {
    return refuseManual(RemoteUpdateResult::Failed, "no update URL configured");
  }
  if (ota_.busy()) {
    return refuseManual(RemoteUpdateResult::Skipped,
                        "local upload in progress");
  }
  powerMode_ = powerModeFor(p_.sys->batteryMv(), cfg_);
  if (powerMode_ == PowerMode::Critical) {  // no radio at all
    return refuseManual(RemoteUpdateResult::Skipped,
                        "battery critical: Wi-Fi not used");
  }
  char err[96] = "";
  if (!p_.net->connect(cfg_, kWifiConnectTimeoutMs, err, sizeof(err))) {
    p_.net->disconnect();
    char msg[96];
    snprintf(msg, sizeof(msg), "Wi-Fi: %.80s", err);
    return refuseManual(RemoteUpdateResult::Failed, msg);
  }
  if (!p_.clock->wallValid()) {
    synchronizeTime();  // certificate dates; not fatal
  }
  manualCheck_ = true;
  manualCheckMonoS_ = p_.clock->monoS();
  manualRun_ = true;
  checkRemoteUpdate(install, true, expect != nullptr ? expect : "");
  manualRun_ = false;
  p_.http->endSession();
  p_.net->disconnect();
  return static_cast<RemoteUpdateResult>(st_->updateResult);
}

void GrainMeter::checkRemoteUpdate(bool install, bool manual,
                                   const char* expect) {
  if (!manual) {
    st_->hasUpdateCheck = 1;
    st_->lastUpdateCheckMonoS = p_.clock->monoS();
    st_->seal();
  }
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
  if (m.serviceId != 0 && !manual) {
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
    if (m.serviceId == 0 || manual) {
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
  if (!install) {
    setUpdateResult(RemoteUpdateResult::Available, "%s available (running %s)",
                    m.version, GM_FW_VERSION);
    return;
  }
  if (expect[0] != '\0' && strcmp(expect, m.version) != 0) {
    // the manifest changed since the user saw it: let them check again
    setUpdateResult(RemoteUpdateResult::Available,
                    "%s available now (not %.32s); check again", m.version,
                    expect);
    return;
  }
  if (powerMode_ != PowerMode::Normal) {
    setUpdateResult(RemoteUpdateResult::Skipped, "%s available, battery low",
                    m.version);
    return;
  }
  GM_LOGI(kTag, "downloading firmware %s", m.version);
  FirmwareSink sink(ota_, p_.ota->slotSize(), *p_.clock,
                    p_.clock->monoS() + kFirmwareDownloadTotalS);
  const HttpResult d =
      p_.http->download(m.url, kFirmwareDownloadTimeoutMs, sink);
  if (!d.transportOk || d.status != 200 || !ota_.busy()) {
    const char* why = sink.timedOut()           ? "took too long"
                      : ota_.error()[0] != '\0' ? ota_.error()
                      : d.transportOk           ? "HTTP error"
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
  otaUpload_.tick();
  const PowerMode mode = powerModeFor(p_.sys->batteryMv(), cfg_);
  if (static_cast<uint8_t>(mode) != st_->alarm.powerMode) {
    st_->alarm.powerMode = static_cast<uint8_t>(mode);
    if (mode != PowerMode::Normal) {
      NotifyEvent e = {p_.clock->wallValid() ? p_.clock->epoch() : 0,
                       p_.clock->monoS(),
                       static_cast<int16_t>(p_.sys->batteryMv()),
                       0,
                       NotifyBattery,
                       static_cast<uint8_t>(mode),
                       0,
                       0};
      alarmEnqueue(st_->alarm, e);
    }
    st_->seal();
  }
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
  const bool alarmDue = alarmWindowDue(st_->alarm, cfg_, p_.clock->monoS());
  if (allowUpload && powerMode_ != PowerMode::Critical &&
      (plan.uploadDue || alarmDue)) {
    // Mark the alarm window even if it coincides with the regular schedule.
    upload(alarmDue);
    if (plan.uploadDue && alarmDue) {
      st_->alarm.samplesSentSinceRegular = 0;
      st_->hasUploadAttempt = 1;
      st_->lastUploadAttemptMonoS = st_->last.monoS;
      st_->seal();
    }
  }
  uint32_t sleep =
      schedulePlan(*st_, cfg_, p_.clock->monoS(), powerMode_).sleepS;
  if (allowUpload && powerMode_ != PowerMode::Critical && st_->alarm.active &&
      cfg_.alarmRepeatMin && st_->alarm.hasWindow) {
    const uint32_t age = p_.clock->monoS() - st_->alarm.lastWindowMonoS;
    const uint32_t interval = cfg_.alarmRepeatMin * 60u;
    const uint32_t wait = age >= interval ? 1 : interval - age;
    if (wait < sleep) sleep = wait;
  }
  return sleep;
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
  p_.sys->configApplied(cfg_);
  GM_LOGI(kTag, "configuration saved");
  return true;
}

// ------------------------------------------------------------- wifi join ----

bool GrainMeter::wifiJoinRequest(const Form& form, char* err, size_t cap) {
  if (joinState_ == JoinState::Pending || joinState_ == JoinState::Running) {
    snprintf(err, cap, "a connection attempt is already running");
    return false;
  }
  char ssid[40] = "";
  if (!form.get("wifi_ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
    snprintf(err, cap, "wifi_ssid required");
    return false;
  }
  Config next = cfg_;
  if (!configApplyForm(form, limits_, &next, err, cap)) {
    return false;
  }
  joinCfg_ = next;
  snprintf(joinSsid_, sizeof(joinSsid_), "%s", next.wifiSsid);
  joinState_ = JoinState::Pending;
  joinMsg_[0] = '\0';
  joinIp_[0] = '\0';
  joinRssi_ = 0;
  GM_LOGI(kTag, "Wi-Fi join requested: '%s'", joinCfg_.wifiSsid);
  return true;
}

bool GrainMeter::wifiJoinTake(Config* candidate) {
  if (joinState_ != JoinState::Pending) {
    return false;
  }
  joinState_ = JoinState::Running;
  *candidate = joinCfg_;
  return true;
}

void GrainMeter::wifiJoinFinish(const Config& candidate, bool ok,
                                const char* msg) {
  joinRssi_ = ok ? p_.net->rssi() : 0;
  snprintf(joinIp_, sizeof(joinIp_), "%s", ok ? p_.net->ipText() : "");
  if (ok) {
    // Only the network fields: the rest may have been edited meanwhile.
    Config next = cfg_;
    memcpy(next.wifiSsid, candidate.wifiSsid, sizeof(next.wifiSsid));
    memcpy(next.wifiPass, candidate.wifiPass, sizeof(next.wifiPass));
    memcpy(next.staticIp, candidate.staticIp, sizeof(next.staticIp));
    memcpy(next.gateway, candidate.gateway, sizeof(next.gateway));
    memcpy(next.netmask, candidate.netmask, sizeof(next.netmask));
    memcpy(next.dns, candidate.dns, sizeof(next.dns));
    if (!configSave(*p_.store, next)) {
      recordError("config: storage write failed");
      snprintf(joinMsg_, sizeof(joinMsg_),
               "connected, but saving the configuration failed");
      joinState_ = JoinState::Failed;
      return;
    }
    cfg_ = next;
    p_.sys->configApplied(cfg_);
    snprintf(joinMsg_, sizeof(joinMsg_), "connected and saved");
    joinState_ = JoinState::Ok;
    GM_LOGI(kTag, "Wi-Fi join: connected to '%s', IP %s, saved", cfg_.wifiSsid,
            joinIp_);
  } else {
    snprintf(joinMsg_, sizeof(joinMsg_), "%s",
             msg != nullptr && msg[0] != '\0' ? msg : "connection failed");
    joinState_ = JoinState::Failed;
    GM_LOGW(kTag, "Wi-Fi join failed: %s", joinMsg_);
  }
  memset(&joinCfg_, 0, sizeof(joinCfg_));  // drop the candidate password
}

void GrainMeter::runWifiJoin() {
  Config candidate;
  if (!wifiJoinTake(&candidate)) {
    return;
  }
  char err[96] = "";
  const bool ok =
      p_.net->connect(candidate, kWifiConnectTimeoutMs, err, sizeof(err));
  wifiJoinFinish(candidate, ok, err);
  memset(&candidate, 0, sizeof(candidate));
}

void GrainMeter::writeWifiJoin(JsonWriter& w) {
  static const char* const kNames[] = {"idle", "pending", "running", "ok",
                                       "failed"};
  w.beginObject()
      .kv("state", kNames[static_cast<int>(joinState_)])
      .kv("ssid", joinSsid_)
      .kv("message", joinMsg_)
      .kv("ip", joinIp_)
      .kv("rssi", static_cast<int64_t>(joinRssi_))
      .endObject();
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
  PointHint hints[kMaxSensors];
  size_t nHints = 0;
  for (size_t i = 0; i < scan_.search.found; ++i) {
    const Rom& rom = scan_.roms[i];
    if (map_.pointOf(rom) != 0 || !familySupported(rom.family())) {
      continue;
    }
    uint8_t raw[kScratchpadLen] = {};
    int16_t centi = 0;
    readSensor(*p_.bus, rom, &centi, raw);
    const uint8_t point = pointFromScratchpad(raw);
    if (point != 0) {
      hints[nHints++] = {rom, point};
    }
  }
  RomMap next = map_;
  uint8_t conflictPoints[kMaxPoints];
  size_t conflicts = 0;
  size_t added =
      assignPointHints(next, hints, nHints, conflictPoints, &conflicts);
  Rom fallback[kMaxSensors];
  size_t nFallback = 0;
  for (size_t i = 0; i < scan_.search.found; ++i) {
    bool conflicting = false;
    for (size_t h = 0; h < nHints; ++h) {
      if (hints[h].rom == scan_.roms[i]) {
        for (size_t c = 0; c < conflicts; ++c) {
          conflicting |= hints[h].point == conflictPoints[c];
        }
      }
    }
    // A duplicate number must not be assigned by the ROM-order fallback.
    if (!conflicting) {
      fallback[nFallback++] = scan_.roms[i];
    }
  }
  added += next.autoAssign(fallback, nFallback);
  if (added > 0 && !saveHintMap(next, hints, nHints)) {
    return 0;
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
      .kv("diag", L.diagResult == DiagSent            ? "sent"
                  : L.diagResult == DiagFailed        ? "failed"
                  : L.diagResult == DiagNotConfigured ? "not_configured"
                                                      : "not_due")
      .kv("http_status", static_cast<int64_t>(L.httpStatus))
      .kvu("epoch", L.epoch)
      .kvu("age_s", p_.clock->monoS() - L.monoS)
      .kvu("samples_sent", L.samplesSent)
      .kvu("samples_confirmed", L.samplesConfirmed)
      .kv("message", L.message)
      .endObject();
}

void GrainMeter::writeAlarm(JsonWriter& w) {
  const AlarmState& a = st_->alarm;
  w.key("alarm")
      .beginObject()
      .kvb("active", a.active != 0)
      .kv("reason", alarmReasonName(a.reason))
      .kvu("point", a.point)
      .key("temp_c")
      .centi(a.centi)
      .key("rise_c")
      .centi(a.riseCenti)
      .kvu("start_epoch", a.startEpoch)
      .kvu("start_mono_s", a.startMonoS)
      .kvu("last_notify_mono_s", a.lastNotifyMonoS)
      .kvu("last_window_mono_s", a.lastWindowMonoS)
      .kvb("pending_window", a.pendingWindow != 0)
      .kvu("pending_events", a.count)
      .kvu("dropped_events", a.dropped)
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
  w.kvu("low_mv", cfg_.battLowMv).kvu("crit_mv", cfg_.battCritMv);
  w.kvb("charging", st_->charging != 0)
      .kvb("frost_charging", st_->frostCharging != 0);
  if (st_->hasBoardTemp) {
    w.key("board_temp_c").centi(st_->boardTempCenti);
  } else {
    w.key("board_temp_c").null();
  }
  w.endObject();
}

void GrainMeter::writeRemoteUpdate(JsonWriter& w) {
  const RemoteUpdateResult r =
      static_cast<RemoteUpdateResult>(st_->updateResult);
  w.key("remote_update")
      .beginObject()
      .kvb("enabled", cfg_.remoteUpdateEnabled())
      .kvb("url_set", cfg_.updateUrl[0] != '\0')
      .kv("running", GM_FW_VERSION)
      .kv("latest", st_->updateLatest)
      .kv("result", remoteUpdateResultName(r))
      .kv("message", st_->updateMessage)
      .kv("rejected", p_.sys->rejectedFirmware());
  // the more recent of the periodic and the manual check
  const uint32_t now = p_.clock->monoS();
  bool have = false;
  uint32_t age = 0;
  if (st_->hasUpdateCheck && now >= st_->lastUpdateCheckMonoS) {
    have = true;
    age = now - st_->lastUpdateCheckMonoS;
  }
  if (manualCheck_ && now >= manualCheckMonoS_ &&
      (!have || now - manualCheckMonoS_ < age)) {
    have = true;
    age = now - manualCheckMonoS_;
  }
  w.key("last_check_age_s");
  if (have) {
    w.unum(age);
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

// One latest sample per UTC hour bounds memory to 169 points (about 1.4 KiB).
static void writeBatteryForecast(JsonWriter& w, HistoryLog& history,
                                 uint32_t now, uint32_t crit,
                                 BatteryForecast* cache, uint32_t* cachedAt) {
  struct Hourly {
    BatterySample pts[169] = {};
    size_t n = 0;
    static bool visit(void* ctx, const HistoryRecord& r) {
      Hourly& h = *static_cast<Hourly*>(ctx);
      if (!r.batteryMv) return true;
      if (h.n && h.pts[h.n - 1].epoch / 3600 == r.epoch / 3600) {
        h.pts[h.n - 1] = {r.epoch, r.batteryMv};
      } else if (h.n < 169)
        h.pts[h.n++] = {r.epoch, r.batteryMv};
      return true;
    }
  } h;
  // status is polled by the portal every few seconds: scanning 7 days of
  // flash history each time is too slow, so the result is reused for 10 min
  // (or until the clock jumps back).
  BatteryForecast f;
  if (now != 0 && *cachedAt != 0 && now >= *cachedAt && now - *cachedAt < 600) {
    f = *cache;
  } else if (now && history.available() &&
             history.forEach(now > 604800 ? now - 604800 : 0, now,
                             Hourly::visit, &h)) {
    // batt_crit_mv = 0 disables the critical mode, not the forecast.
    f = batteryForecast(h.pts, h.n, now, crit != 0 ? crit : 3500);
    *cache = f;
    *cachedAt = now;
  }
  const char* states[] = {"unknown", "discharging", "charging", "stable"};
  w.key("battery_forecast").beginObject().kv("state", states[f.state]);
  w.key("days");
  if (f.days < 0)
    w.null();
  else
    w.num(f.days);
  w.kv("slope_mv_per_day", f.slopeMvPerDay)
      .kvu("samples", f.samples)
      .endObject();
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
  const int32_t mah = p_.sys->batteryMahPerDayCenti();
  const int soc = batteryPercent(mv);
  const int32_t days = batteryRuntimeCentiDays(mv, cfg_.battCapacityMah, mah);
  w.key("battery_runtime").beginObject().key("mah_per_day");
  if (mah < 0)
    w.null();
  else
    w.centi(mah);
  w.kvu("capacity_mah", cfg_.battCapacityMah).key("soc_pct");
  if (soc < 0)
    w.null();
  else
    w.num(soc);
  w.key("days");
  if (days < 0)
    w.null();
  else
    w.centi(days);
  w.endObject();
  w.key("wifi")
      .beginObject()
      .kv("state", p_.net->stateText())
      .kv("ssid", cfg_.wifiSsid)
      .kv("rssi", static_cast<int64_t>(p_.net->rssi()))
      .kv("ip", p_.net->ipText())
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
  writeAlarm(w);
  writePower(w);
  writeBatteryForecast(w, history_,
                       p_.clock->wallValid() ? p_.clock->epoch() : 0,
                       cfg_.battCritMv, &forecast_, &forecastAt_);
  writeRemoteUpdate(w);
  writeErrors(w);
  w.key("history").beginObject().kvb("available", history_.available());
  w.endObject();
  writeRemote(w);
  w.key("ota");
  ota_.writeJson(w);
  p_.sys->writePlatformJson(w, false);
  w.endObject();
}

uint32_t GrainMeter::lastMeasureEpoch() const {
  if (!st_->hasMeasured || !p_.clock->wallValid()) return 0;
  const uint32_t mono = p_.clock->monoS();
  const uint32_t now = p_.clock->epoch();
  const uint32_t ago =
      mono >= st_->lastMeasureMonoS ? mono - st_->lastMeasureMonoS : 0;
  return ago < now ? now - ago : 0;
}

uint32_t GrainMeter::historyMaxGapS() const {
  // Longest interval the scheduler uses (critical power mode), two of them,
  // plus a margin for wake-up jitter.
  const uint32_t interval = cfg_.measureIntervalS * kCriticalMeasureFactor;
  return history_.policy().heartbeatS + 2 * interval + 60;
}

void GrainMeter::writeHistoryInfo(JsonWriter& w) {
  w.beginObject().kvb("available", history_.available());
  HistoryStats hs;
  if (history_.available() && history_.stats(&hs)) {
    const HistoryPolicy& pol = history_.policy();
    w.kvu("records", hs.records)
        .kvu("used_bytes", hs.usedBytes)
        .kvu("capacity_bytes", hs.capacityBytes)
        .kvu("oldest_epoch", hs.oldestEpoch)
        .kvu("newest_epoch", hs.newestEpoch)
        .kvu("last_measure_epoch", lastMeasureEpoch())
        .kvu("heartbeat_s", pol.heartbeatS)
        .kvu("deadband_centi", pol.deadbandCenti)
        .kvu("max_gap_s", historyMaxGapS());
  }
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
  writeAlarm(w);
  writePower(w);
  writeClock(w);
  writeBatteryForecast(w, history_,
                       p_.clock->wallValid() ? p_.clock->epoch() : 0,
                       cfg_.battCritMv, &forecast_, &forecastAt_);
  writeRemoteUpdate(w);
  writeErrors(w);
  w.key("history");
  writeHistoryInfo(w);
  writeRemote(w);
  w.key("ota");
  ota_.writeJson(w);
  p_.sys->writePlatformJson(w, true);
  w.endObject();
}

}  // namespace gm
