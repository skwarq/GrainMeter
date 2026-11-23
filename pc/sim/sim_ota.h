// SPDX-License-Identifier: MIT
#ifndef GM_PC_SIM_OTA_H
#define GM_PC_SIM_OTA_H

#include <cstdint>
#include <fstream>
#include <string>

#include "ota/ota_session.h"

namespace gmpc {

// Simulated A/B slots: the image is streamed into <dataDir>/ota_<slot>.bin,
// "validated" (size and header already checked by OtaSession) and selected
// for the next boot. restart() swaps the running slot like the bootloader.
class SimOtaTarget : public gm::OtaTarget {
 public:
  explicit SimOtaTarget(const std::string& dataDir);

  size_t slotSize() override { return kSlotSize; }
  const char* slotName() override { return running_ == 0 ? "ota_1" : "ota_0"; }
  bool begin(size_t imageSize, char* err, size_t cap) override;
  bool write(const uint8_t* data, size_t len, char* err, size_t cap) override;
  bool finish(char* err, size_t cap) override;
  void abort() override;

  // Simulated reboot: returns true if a new image became active.
  // newVersion: version of the pending image (for rollback bookkeeping).
  bool restart(const std::string& newVersion);
  // Next new image "crashes" before confirming itself -> bootloader rollback.
  void setFailNextBoot(bool on) { failNextBoot_ = on; }
  bool failNextBoot() const { return failNextBoot_; }
  const std::string& rejectedVersion() const { return rejected_; }
  const char* runningSlot() const { return running_ == 0 ? "ota_0" : "ota_1"; }
  bool pending() const { return pending_; }
  const std::string& lastBooted() const { return lastBooted_; }
  void setFailWrites(bool on) { failWrites_ = on; }

  // Same as firmware/partitions.csv (ota_0 / ota_1 size).
  static const size_t kSlotSize = 0x1D0000;

 private:
  std::string dataDir_;
  std::ofstream out_;
  size_t expected_;
  size_t written_;
  int running_;
  bool pending_;
  bool failWrites_;
  std::string lastBooted_;
  bool failNextBoot_ = false;
  std::string rejected_;
};

}  // namespace gmpc

#endif
