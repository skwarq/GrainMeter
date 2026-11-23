// SPDX-License-Identifier: MIT
#include "app/portal.h"

#include <stdio.h>
#include <string.h>

#include "util/log.h"

namespace gm {

static const char* kTag = "portal";
static const unsigned kMaxAuthFails = 5;
static const uint32_t kLockoutS = 60;

void Portal::reply(PortalReply* r, int status) {
  r->status = status;
  r->contentType = "application/json";
  r->body = out_;
  r->len = strlen(out_);
}

void Portal::replyError(PortalReply* r, int status, const char* msg) {
  JsonWriter w(out_, sizeof(out_));
  w.beginObject().kvb("ok", false).kv("error", msg).endObject();
  reply(r, status);
}

void Portal::replyOk(PortalReply* r, const char* msg) {
  JsonWriter w(out_, sizeof(out_));
  w.beginObject().kvb("ok", true).kv("message", msg).endObject();
  reply(r, 200);
}

bool Portal::authorized(const char* password) {
  const uint32_t now = app_.ports().clock->monoS();
  if (lockUntilS_ != 0 && now < lockUntilS_) {
    return false;
  }
  if (app_.checkServicePassword(password)) {
    authFails_ = 0;
    lockUntilS_ = 0;
    return true;
  }
  if (++authFails_ >= kMaxAuthFails) {
    lockUntilS_ = now + kLockoutS;
    authFails_ = 0;
    GM_LOGW(kTag, "too many wrong passwords: writes locked for %u s",
            static_cast<unsigned>(kLockoutS));
  }
  return false;
}

static bool is(const PortalRequest& r, const char* method, const char* path) {
  return strcmp(r.method, method) == 0 && strcmp(r.path, path) == 0;
}

// "<from>/<to>" with decimal epoch seconds, from <= to.
static bool parseRange(const char* p, uint32_t* from, uint32_t* to) {
  uint32_t v[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    if (*p < '0' || *p > '9') return false;
    uint64_t x = 0;
    while (*p >= '0' && *p <= '9') {
      x = x * 10 + static_cast<uint64_t>(*p++ - '0');
      if (x > 0xFFFFFFFFull) return false;
    }
    v[k] = static_cast<uint32_t>(x);
    if (k == 0 && *p++ != '/') return false;
  }
  if (*p != '\0' || v[0] > v[1]) return false;
  *from = v[0];
  *to = v[1];
  return true;
}

bool Portal::handle(const PortalRequest& req, PortalReply* r) {
  r->restartAfter = false;
  if (strncmp(req.path, "/api/", 5) != 0) {
    return false;
  }
  // ---- read-only ----
  if (is(req, "GET", "/api/status")) {
    JsonWriter w(out_, sizeof(out_));
    app_.writeStatus(w);
    if (!w.ok()) {
      replyError(r, 500, "status too large");
    } else {
      reply(r, 200);
    }
    return true;
  }
  if (is(req, "GET", "/api/diag")) {
    JsonWriter w(out_, sizeof(out_));
    app_.writeDiag(w);
    if (!w.ok()) {
      replyError(r, 500, "diagnostics too large");
    } else {
      reply(r, 200);
    }
    return true;
  }
  if (is(req, "GET", "/api/config")) {
    JsonWriter w(out_, sizeof(out_));
    configWriteJson(w, app_.config());
    reply(r, 200);
    return true;
  }
  if (is(req, "GET", "/api/history")) {
    JsonWriter w(out_, sizeof(out_));
    app_.writeHistoryInfo(w);
    reply(r, 200);
    return true;
  }
  if (strcmp(req.method, "GET") == 0 &&
      strncmp(req.path, "/api/history/", 13) == 0) {
    // /api/history/<from>/<to> (UTC epoch seconds): one page of the local
    // history, binary (see historyExport). The client asks per day and
    // continues from header.next while it is not 0.
    uint32_t from = 0;
    uint32_t to = 0;
    if (!parseRange(req.path + 13, &from, &to)) {
      replyError(r, 404, "use /api/history/<from>/<to> (epoch seconds)");
      return true;
    }
    if (!app_.history().available()) {
      replyError(r, 503, "local history not available");
      return true;
    }
    HistoryExportRequest q;
    q.from = from;
    q.to = to;
    q.maxRows = kHistoryPageRows;
    q.maxGapS = app_.historyMaxGapS();
    q.holdUntil = app_.lastMeasureEpoch();
    const size_t len = historyExport(
        app_.history(), q, reinterpret_cast<uint8_t*>(out_), sizeof(out_));
    if (len == 0) {
      replyError(r, 500, "history read failed");
      return true;
    }
    r->status = 200;
    r->contentType = "application/octet-stream";
    r->body = out_;
    r->len = len;
    return true;
  }
  if (is(req, "GET", "/api/ota")) {
    JsonWriter w(out_, sizeof(out_));
    app_.ota().writeJson(w);
    reply(r, 200);
    return true;
  }
  if (strcmp(req.method, "POST") != 0) {
    replyError(r, 404, "unknown endpoint");
    return true;
  }
  // ---- state changing: password required ----
  if (req.bodyLen > kPortalMaxBody) {
    replyError(r, 413, "request body too large");
    return true;
  }
  if (!authorized(req.password)) {
    replyError(r, 401,
               app_.config().servicePassword[0] == '\0'
                   ? "service password not set (use USB console)"
                   : "wrong or missing service password");
    return true;
  }
  const Form form(req.body != nullptr ? req.body : "",
                  req.body != nullptr ? req.bodyLen : 0);
  char err[96] = "";

  if (is(req, "POST", "/api/login")) {
    replyOk(r, "authorized");
  } else if (is(req, "POST", "/api/config")) {
    if (app_.ota().busy()) {
      replyError(r, 409, "update in progress");
    } else if (app_.applyConfig(form, err, sizeof(err))) {
      replyOk(r, "configuration saved");
    } else {
      replyError(r, 400, err);
    }
  } else if (is(req, "POST", "/api/scan")) {
    app_.scan();
    JsonWriter w(out_, sizeof(out_));
    app_.writeDiag(w);
    reply(r, w.ok() ? 200 : 500);
  } else if (is(req, "POST", "/api/measure")) {
    app_.measure(form.has("store"));
    JsonWriter w(out_, sizeof(out_));
    app_.writeStatus(w);
    reply(r, w.ok() ? 200 : 500);
  } else if (is(req, "POST", "/api/upload")) {
    const LastUpload& L = app_.upload();
    JsonWriter w(out_, sizeof(out_));
    w.beginObject()
        .kvb("ok", L.ok != 0)
        .kv("message", L.message)
        .kv("http_status", static_cast<int64_t>(L.httpStatus));
    w.kvu("samples_confirmed", L.samplesConfirmed).endObject();
    reply(r, 200);
  } else if (is(req, "POST", "/api/map")) {
    char rom[40];
    long point = 0;
    long height = -1;
    if (!form.get("rom", rom, sizeof(rom)) ||
        !form.getInt("point", 1, kMaxPoints, &point)) {
      replyError(r, 400, "rom and point (1..8) required");
    } else if (form.has("height_cm") &&
               !form.getInt("height_cm", -1, 32767, &height)) {
      replyError(r, 400, "height_cm must be -1..32767");
    } else if (app_.mapAssign(rom, point, height, err, sizeof(err))) {
      replyOk(r, "mapping saved");
    } else {
      replyError(r, 400, err);
    }
  } else if (is(req, "POST", "/api/unmap")) {
    char rom[40];
    if (!form.get("rom", rom, sizeof(rom))) {
      replyError(r, 400, "rom required");
    } else if (app_.mapRemove(rom, err, sizeof(err))) {
      replyOk(r, "mapping removed");
    } else {
      replyError(r, 400, err);
    }
  } else if (is(req, "POST", "/api/automap")) {
    char msg[48];
    snprintf(msg, sizeof(msg), "%u sensor(s) assigned",
             static_cast<unsigned>(app_.mapAuto()));
    replyOk(r, msg);
  } else if (is(req, "POST", "/api/restart")) {
    if (app_.ota().busy()) {
      replyError(r, 409, "update in progress");
    } else {
      replyOk(r, "restarting");
      r->restartAfter = true;
    }
  } else if (is(req, "POST", "/api/service/exit")) {
    char msg[96] = "";
    if (app_.ota().busy()) {
      replyError(r, 409, "update in progress");
    } else if (app_.ports().sys->platformAction("exit_service", form, msg,
                                                sizeof(msg))) {
      replyOk(r, msg);
    } else {
      replyError(r, 400, msg[0] ? msg : "not supported");
    }
  } else if (is(req, "POST", "/api/sim")) {
    char msg[96] = "";
    if (app_.ports().sys->platformAction("sim", form, msg, sizeof(msg))) {
      replyOk(r, msg);
    } else {
      replyError(r, 400,
                 msg[0] ? msg : "simulation not available on this platform");
    }
  } else {
    replyError(r, 404, "unknown endpoint");
  }
  return true;
}

bool Portal::multipartSink(void* ctx, const uint8_t* data, size_t len) {
  return static_cast<Portal*>(ctx)->app_.ota().feed(data, len);
}

bool Portal::otaStart(const char* password, size_t contentLength, OtaBody kind,
                      const char* contentType, const char* fileName,
                      PortalReply* r) {
  r->restartAfter = false;
  if (!authorized(password)) {
    replyError(r, 401, "wrong or missing service password");
    return false;
  }
  OtaSession& ota = app_.ota();
  useMultipart_ = kind == OtaBody::Multipart;
  if (useMultipart_ && !multipart_.begin(contentType, multipartSink, this)) {
    replyError(r, 400, multipart_.error());
    return false;
  }
  if (!ota.start(contentLength, kind == OtaBody::Raw, fileName)) {
    replyError(r, ota.httpStatus(), ota.error());
    return false;
  }
  return true;
}

bool Portal::otaFeed(const uint8_t* data, size_t len) {
  if (useMultipart_) {
    if (!multipart_.feed(data, len)) {
      if (app_.ota().busy()) {
        app_.ota().abort(multipart_.error() != nullptr ? multipart_.error()
                                                       : "malformed upload");
      }
      return false;
    }
    return app_.ota().busy();
  }
  return app_.ota().feed(data, len);
}

void Portal::otaAbort(const char* reason) { app_.ota().abort(reason); }

void Portal::otaFinish(PortalReply* r) {
  OtaSession& ota = app_.ota();
  r->restartAfter = false;
  if (ota.state() == OtaSession::State::Receiving) {
    if (useMultipart_ && !multipart_.fileComplete()) {
      ota.abort("upload incomplete (file part not terminated)");
    } else {
      ota.finish();
    }
  }
  if (ota.state() != OtaSession::State::Done) {
    replyError(r, ota.httpStatus(),
               ota.error()[0] ? ota.error() : "update failed");
    return;
  }
  JsonWriter w(out_, sizeof(out_));
  w.beginObject()
      .kvb("ok", true)
      .kv("message", "image verified and selected for next boot; restarting")
      .kv("version", ota.image().version)
      .kvu("bytes", ota.received())
      .endObject();
  reply(r, 200);
  r->restartAfter = true;
}

}  // namespace gm
