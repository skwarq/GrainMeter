// SPDX-License-Identifier: MIT
#include "platform/history_file.h"

#include <cstring>
#include <fstream>

namespace gmpc {

FileHistoryStorage::FileHistoryStorage(const std::string& path)
    : path_(path), data_(kSize, 0xFF) {
  std::ifstream in(path_, std::ios::binary);
  if (in) {
    in.read(reinterpret_cast<char*>(data_.data()),
            static_cast<std::streamsize>(kSize));
  }
}

bool FileHistoryStorage::flush(size_t offset, size_t len) {
  std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
  if (!f) {
    // first use: create the whole image
    std::ofstream create(path_, std::ios::binary | std::ios::trunc);
    create.write(reinterpret_cast<const char*>(data_.data()),
                 static_cast<std::streamsize>(kSize));
    return static_cast<bool>(create);
  }
  f.seekp(static_cast<std::streamoff>(offset));
  f.write(reinterpret_cast<const char*>(data_.data() + offset),
          static_cast<std::streamsize>(len));
  return static_cast<bool>(f);
}

bool FileHistoryStorage::read(size_t offset, void* buf, size_t len) {
  std::lock_guard<std::mutex> lock(mu_);
  if (offset + len > kSize) return false;
  std::memcpy(buf, data_.data() + offset, len);
  return true;
}

bool FileHistoryStorage::write(size_t offset, const void* buf, size_t len) {
  std::lock_guard<std::mutex> lock(mu_);
  if (offset + len > kSize) return false;
  const uint8_t* src = static_cast<const uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) {
    data_[offset + i] &= src[i];  // NOR flash: bits can only go 1 -> 0
  }
  return flush(offset, len);
}

bool FileHistoryStorage::eraseSector(size_t offset) {
  std::lock_guard<std::mutex> lock(mu_);
  if (offset % kSector != 0 || offset + kSector > kSize) return false;
  std::memset(data_.data() + offset, 0xFF, kSector);
  return flush(offset, kSector);
}

}  // namespace gmpc
