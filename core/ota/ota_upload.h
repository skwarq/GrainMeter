// SPDX-License-Identifier: MIT
#ifndef GM_OTA_UPLOAD_H
#define GM_OTA_UPLOAD_H

#include "ota/ota_session.h"
#include "util/sha256.h"

namespace gm {
class Clock;

// Chunked upload owns the shared session only between begin and finish/abort.
// Call tick from the platform loop even while ota().busy() prevents sleeping.
class OtaUpload {
 public:
  enum { kChunkSize = 1440, kTimeoutS = 120 };
  // Firmware can refuse begin (e.g. 409 session_short) and extend its remote
  // session on acceptance. The callback runs after size/hash validation.
  typedef bool (*CanBegin)(void* ctx, int* status, const char** error);
  OtaUpload(OtaSession& session, Clock& clock);
  void setCanBegin(CanBegin callback, void* ctx) {
    canBegin_ = callback;
    context_ = ctx;
  }
  bool begin(size_t size, const char* sha256hex, const char* name,
             bool critical);
  bool chunk(size_t off, const uint8_t* data, size_t len);
  bool finish(const char* sha256hex);
  void abort();
  void tick();
  bool active() const { return active_ && session_.busy(); }
  int httpStatus() const { return status_; }
  const char* error() const { return error_; }
  size_t next() const { return session_.received(); }

 private:
  bool reject(int status, const char* error);
  bool sessionResult(bool ok);
  OtaSession& session_;
  Clock& clock_;
  Sha256 sha_;
  uint8_t expectedHash_[32];
  bool active_;
  uint32_t lastChunkS_;
  int status_;
  char error_[96];
  CanBegin canBegin_;
  void* context_;
};
}  // namespace gm
#endif
