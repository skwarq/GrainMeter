// SPDX-License-Identifier: MIT
#include "ota/ota_upload.h"

#include <stdio.h>
#include <string.h>

#include "app/platform.h"
#include "util/log.h"

namespace gm {
OtaUpload::OtaUpload(OtaSession& session, Clock& clock)
    : session_(session),
      clock_(clock),
      active_(false),
      lastChunkS_(0),
      status_(200),
      canBegin_(nullptr),
      context_(nullptr) {
  memset(expectedHash_, 0, sizeof(expectedHash_));
  error_[0] = '\0';
}

bool OtaUpload::reject(int status, const char* error) {
  status_ = status;
  snprintf(error_, sizeof(error_), "%s", error);
  GM_LOGW("ota", "chunked upload: %s", error_);
  return false;
}

bool OtaUpload::sessionResult(bool ok) {
  if (!ok) {
    active_ = false;
    return reject(session_.httpStatus(), session_.error());
  }
  status_ = 200;
  error_[0] = '\0';
  return true;
}

void OtaUpload::tick() {
  if (!active_) return;
  if (!session_.busy()) {
    active_ = false;
  } else if (static_cast<uint32_t>(clock_.monoS() - lastChunkS_) >= kTimeoutS) {
    session_.abort("upload timeout");
    active_ = false;
  }
}

bool OtaUpload::begin(size_t size, const char* sha256hex, const char* name,
                      bool critical) {
  tick();
  if (critical) return reject(503, "low_battery");
  if (session_.busy()) return reject(409, "busy");
  uint8_t hash[32];
  if (sha256hex == nullptr || !sha256FromHex(sha256hex, hash)) {
    return reject(400, "sha256");
  }
  if (!sessionResult(session_.start(size, true, name))) return false;
  if (canBegin_) {
    int status = 409;
    const char* error = "session_short";
    if (!canBegin_(context_, &status, &error)) {
      session_.abort(error);
      return reject(status, error);
    }
  }
  memcpy(expectedHash_, hash, sizeof(hash));
  sha_.reset();
  lastChunkS_ = clock_.monoS();
  active_ = true;
  return true;
}

bool OtaUpload::chunk(size_t off, const uint8_t* data, size_t len) {
  tick();
  if (!active()) return reject(409, session_.busy() ? "busy" : "no_upload");
  if (len == 0 || len > kChunkSize || data == nullptr) {
    return reject(400, "chunk");
  }
  const size_t received = session_.received();
  if (off > received || (off < received && len > received - off)) {
    return reject(409, "offset");
  }
  if (off == received) {
    if (!sessionResult(session_.feed(data, len))) return false;
    sha_.update(data, len);
  }
  lastChunkS_ = clock_.monoS();
  status_ = 200;
  error_[0] = '\0';
  return true;
}

bool OtaUpload::finish(const char* sha256hex) {
  tick();
  if (!active()) return reject(409, session_.busy() ? "busy" : "no_upload");
  uint8_t hash[32], actual[32];
  const bool valid = sha256hex != nullptr && sha256FromHex(sha256hex, hash);
  sha_.finish(actual);
  if (!valid || memcmp(hash, expectedHash_, sizeof(hash)) != 0 ||
      memcmp(actual, expectedHash_, sizeof(actual)) != 0) {
    abort();
    return reject(422, "sha256");
  }
  active_ = false;
  return sessionResult(session_.finish());
}

void OtaUpload::abort() {
  if (active_) session_.abort("upload aborted");
  active_ = false;
  status_ = 200;
  error_[0] = '\0';
}
}  // namespace gm
