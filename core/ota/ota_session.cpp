// SPDX-License-Identifier: MIT
#include "ota/ota_session.h"

#include <stdio.h>
#include <string.h>

#include "util/log.h"

namespace gm {

static const char* kTag = "ota";

void OtaSession::reset() {
  state_ = State::Idle;
  expected_ = 0;
  exact_ = true;
  received_ = 0;
  fileName_[0] = '\0';
  headerChecked_ = false;
  probeLen_ = 0;
  memset(&info_, 0, sizeof(info_));
  error_[0] = '\0';
  httpStatus_ = 200;
}

bool OtaSession::fail(int status, const char* msg) {
  if (state_ == State::Receiving && headerChecked_) {
    target_.abort();
  }
  state_ = State::Failed;
  httpStatus_ = status;
  snprintf(error_, sizeof(error_), "%s", msg);
  GM_LOGE(kTag, "update failed: %s", msg);
  return false;
}

bool OtaSession::start(size_t contentLength, bool exactLength,
                       const char* fileName) {
  if (state_ == State::Receiving) {
    httpStatus_ = 409;
    snprintf(error_, sizeof(error_), "another update is in progress");
    return false;  // do not disturb the running session
  }
  reset();
  state_ = State::Receiving;
  snprintf(fileName_, sizeof(fileName_), "%s",
           fileName != nullptr ? fileName : "");
  if (contentLength == 0) {
    return fail(411, "missing Content-Length");
  }
  if (exactLength && contentLength < kMinImageLen) {
    return fail(400, "file too small to be an application image");
  }
  // Multipart framing adds a few hundred bytes around the file.
  const size_t limit = target_.slotSize() + (exactLength ? 0 : 4096);
  if (contentLength > limit) {
    return fail(413, "file larger than the OTA slot");
  }
  expected_ = exactLength
                  ? contentLength
                  : (contentLength < target_.slotSize() ? contentLength
                                                        : target_.slotSize());
  exact_ = exactLength;
  GM_LOGI(kTag, "receiving '%s' (%u bytes) for %s", fileName_,
          static_cast<unsigned>(contentLength), target_.slotName());
  return true;
}

bool OtaSession::feed(const uint8_t* data, size_t len) {
  if (state_ != State::Receiving) {
    return false;
  }
  if (received_ + len > expected_) {
    return fail(exact_ ? 400 : 413,
                exact_ ? "more data than declared Content-Length"
                       : "file larger than the OTA slot");
  }
  received_ += len;
  char err[96];
  if (!headerChecked_) {
    const size_t take =
        (kImageProbeLen - probeLen_ < len) ? kImageProbeLen - probeLen_ : len;
    memcpy(probe_ + probeLen_, data, take);
    probeLen_ += take;
    data += take;
    len -= take;
    if (probeLen_ < kImageProbeLen) {
      return true;
    }
    const char* why = nullptr;
    if (!otaInspectImage(probe_, probeLen_, chipId_, project_, &info_, &why)) {
      return fail(415, why);
    }
    headerChecked_ = true;
    if (!target_.begin(expected_, err, sizeof(err))) {
      headerChecked_ = false;  // nothing to abort
      return fail(500, err);
    }
    if (!target_.write(probe_, probeLen_, err, sizeof(err))) {
      return fail(500, err);
    }
  }
  if (len > 0 && !target_.write(data, len, err, sizeof(err))) {
    return fail(500, err);
  }
  return true;
}

bool OtaSession::finish() {
  if (state_ != State::Receiving) {
    return false;
  }
  if (!headerChecked_ || (exact_ && received_ != expected_) ||
      received_ < kMinImageLen) {
    return fail(400, "upload incomplete");
  }
  char err[96];
  if (!target_.finish(err, sizeof(err))) {
    state_ = State::Failed;  // target already cleaned up
    httpStatus_ = 422;
    snprintf(error_, sizeof(error_), "%s", err);
    GM_LOGE(kTag, "image rejected: %s", err);
    return false;
  }
  state_ = State::Done;
  httpStatus_ = 200;
  GM_LOGI(kTag, "image %s %s written to %s", info_.project, info_.version,
          target_.slotName());
  return true;
}

void OtaSession::abort(const char* reason) {
  if (state_ == State::Receiving) {
    fail(400, reason);
  }
}

void OtaSession::writeJson(JsonWriter& w, size_t chunk) const {
  static const char* const kStates[] = {"idle", "receiving", "done", "failed"};
  w.beginObject()
      .kv("state", kStates[static_cast<int>(state_)])
      .kvu("received", received_)
      .kvu("expected", expected_)
      .kv("file", fileName_)
      .kv("error", error_);
  if (chunk) w.kvu("chunk", chunk);
  if (headerChecked_) {
    w.kv("version", info_.version)
        .kv("project", info_.project)
        .kv("idf", info_.idfVersion);
  }
  w.endObject();
}

}  // namespace gm
