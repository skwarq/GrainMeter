// SPDX-License-Identifier: MIT
#ifndef GM_OTA_SESSION_H
#define GM_OTA_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "ota/ota_image.h"
#include "util/json.h"

namespace gm {

// Writes an image into the inactive slot. ESP32: esp_ota_begin/write/end +
// esp_ota_set_boot_partition. PC: file + simulated restart.
class OtaTarget {
 public:
  virtual ~OtaTarget() {}
  virtual size_t slotSize() = 0;       // capacity of the inactive slot
  virtual const char* slotName() = 0;  // e.g. "ota_1"
  virtual bool begin(size_t imageSize, char* err, size_t cap) = 0;
  virtual bool write(const uint8_t* data, size_t len, char* err,
                     size_t cap) = 0;
  // Validates the complete image and only then selects it for next boot.
  virtual bool finish(char* err, size_t cap) = 0;
  virtual void abort() = 0;  // boot partition stays unchanged
};

// Streaming upload state machine used by both HTTP servers:
//   start(len, exact, filename) -> feed(chunk)* -> finish()   (or abort())
// Any file chosen in the browser is accepted; validity is decided by its
// content (ESP-IDF image header, chip, project name, size), never by the
// file name. Nothing is written until the header has been validated.
class OtaSession {
 public:
  enum class State { Idle, Receiving, Done, Failed };

  OtaSession(OtaTarget& target, uint16_t chipId, const char* project)
      : target_(target), chipId_(chipId), project_(project) {
    reset();
  }

  // exactLength=true: contentLength is the image size (raw body).
  // exactLength=false: contentLength is an upper bound (multipart body).
  // Returns false with error() set; HTTP status in httpStatus().
  bool start(size_t contentLength, bool exactLength, const char* fileName);
  bool feed(const uint8_t* data, size_t len);
  bool finish();
  void abort(const char* reason);

  State state() const { return state_; }
  bool busy() const { return state_ == State::Receiving; }
  const char* error() const { return error_; }
  int httpStatus() const { return httpStatus_; }
  size_t received() const { return received_; }
  size_t expected() const { return expected_; }
  const char* fileName() const { return fileName_; }
  const ImageInfo& image() const { return info_; }
  bool imageValid() const { return headerChecked_; }

  void writeJson(JsonWriter& w, size_t chunk = 0) const;  // object
  void reset();

 private:
  bool fail(int status, const char* msg);

  OtaTarget& target_;
  uint16_t chipId_;
  const char* project_;
  State state_;
  size_t expected_;
  bool exact_;
  size_t received_;
  char fileName_[64];
  bool headerChecked_;
  uint8_t probe_[kImageProbeLen];
  size_t probeLen_;
  ImageInfo info_;
  char error_[96];
  int httpStatus_;
};

}  // namespace gm

#endif
