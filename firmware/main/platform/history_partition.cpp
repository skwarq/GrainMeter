// SPDX-License-Identifier: MIT
#include "platform/history_partition.h"

#include "esp_log.h"

static const char* kTag = "history";
static const esp_partition_subtype_t kSubtype =
    static_cast<esp_partition_subtype_t>(0x40);

bool HistoryPartition::open() {
  part_ =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kSubtype, "history");
  if (part_ == nullptr) {
    ESP_LOGW(
        kTag,
        "no 'history' partition (older partition table): local history off");
    return false;
  }
  ESP_LOGI(kTag, "history partition: %lu KiB at 0x%lx",
           static_cast<unsigned long>(part_->size / 1024),
           static_cast<unsigned long>(part_->address));
  return true;
}

bool HistoryPartition::read(size_t offset, void* buf, size_t len) {
  return part_ != nullptr &&
         esp_partition_read(part_, offset, buf, len) == ESP_OK;
}

bool HistoryPartition::write(size_t offset, const void* buf, size_t len) {
  return part_ != nullptr &&
         esp_partition_write(part_, offset, buf, len) == ESP_OK;
}

bool HistoryPartition::eraseSector(size_t offset) {
  return part_ != nullptr &&
         esp_partition_erase_range(part_, offset, sectorSize()) == ESP_OK;
}
