// SPDX-License-Identifier: MIT
#include "sim/sim_ota.h"

#include <cstdio>

namespace gmpc {

SimOtaTarget::SimOtaTarget(const std::string& dataDir)
    : dataDir_(dataDir),
      expected_(0),
      written_(0),
      running_(0),
      pending_(false),
      failWrites_(false) {}

bool SimOtaTarget::begin(size_t imageSize, char* err, size_t cap) {
  const std::string path = dataDir_ + "/" + slotName() + ".bin";
  out_.close();
  out_.open(path, std::ios::binary | std::ios::trunc);
  if (!out_) {
    std::snprintf(err, cap, "cannot open %s", path.c_str());
    return false;
  }
  expected_ = imageSize;
  written_ = 0;
  pending_ =
      false;  // like esp_ota_begin: an older pending image is invalidated
  return true;
}

bool SimOtaTarget::write(const uint8_t* data, size_t len, char* err,
                         size_t cap) {
  if (failWrites_) {
    std::snprintf(err, cap, "flash write failed (simulated)");
    return false;
  }
  out_.write(reinterpret_cast<const char*>(data),
             static_cast<std::streamsize>(len));
  if (!out_) {
    std::snprintf(err, cap, "write to slot file failed");
    return false;
  }
  written_ += len;
  return true;
}

bool SimOtaTarget::finish(char* err, size_t cap) {
  out_.close();
  // imageSize given to begin() is an upper bound for multipart uploads
  // (like esp_ota_begin); the device verifies the image itself in
  // esp_ota_end, here only the bounds are checked.
  if (written_ > expected_ || written_ < 4096) {
    std::snprintf(err, cap, "image size invalid (%zu bytes)", written_);
    return false;
  }
  pending_ = true;  // esp_ota_set_boot_partition() equivalent
  return true;
}

void SimOtaTarget::abort() {
  out_.close();
  pending_ = false;  // boot partition unchanged
}

bool SimOtaTarget::restart(const std::string& newVersion) {
  if (!pending_) {
    lastBooted_ = std::string(runningSlot()) + " (unchanged)";
    return false;
  }
  pending_ = false;
  if (failNextBoot_) {
    // New image never confirmed itself: like ESP-IDF's rollback the
    // bootloader keeps the previous slot and the version is remembered.
    failNextBoot_ = false;
    rejected_ = newVersion;
    lastBooted_ = std::string(runningSlot()) + " (rollback: " + newVersion +
                  " failed its first start)";
    return false;
  }
  running_ = 1 - running_;
  lastBooted_ = std::string(runningSlot()) + " (" + newVersion +
                ", confirmed after self-check)";
  return true;
}

}  // namespace gmpc
