// SPDX-License-Identifier: MIT
#ifndef GM_PC_HISTORY_FILE_H
#define GM_PC_HISTORY_FILE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "data/history_log.h"

namespace gmpc {

// Emulated NOR flash for the local history (same size and sector layout as
// the ESP32 "history" partition): erase sets a sector to 0xFF, write can
// only clear bits. Kept in memory and mirrored to <dataDir>/history.bin, so
// the history survives simulator restarts like flash does.
class FileHistoryStorage : public gm::HistoryStorage {
 public:
  static const size_t kSize = 0x30000;  // = firmware/partitions.csv
  static const size_t kSector = 4096;

  explicit FileHistoryStorage(const std::string& path);

  size_t size() override { return kSize; }
  size_t sectorSize() override { return kSector; }
  bool read(size_t offset, void* buf, size_t len) override;
  bool write(size_t offset, const void* buf, size_t len) override;
  bool eraseSector(size_t offset) override;

 private:
  bool flush(size_t offset, size_t len);

  std::mutex mu_;
  std::string path_;
  std::vector<uint8_t> data_;
};

}  // namespace gmpc

#endif
