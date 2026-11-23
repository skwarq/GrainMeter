// SPDX-License-Identifier: MIT
#pragma once

#include "data/history_log.h"
#include "esp_partition.h"

// Local measurement history in the "history" data partition (subtype 0x40,
// see partitions.csv). Missing on devices flashed with an older partition
// table: then open() fails and the history is simply disabled (OTA cannot
// change the partition table - a USB flash adds it).
class HistoryPartition : public gm::HistoryStorage {
 public:
  bool open();
  size_t size() override { return part_ != nullptr ? part_->size : 0; }
  size_t sectorSize() override { return 4096; }
  bool read(size_t offset, void* buf, size_t len) override;
  bool write(size_t offset, const void* buf, size_t len) override;
  bool eraseSector(size_t offset) override;

 private:
  const esp_partition_t* part_ = nullptr;
};
