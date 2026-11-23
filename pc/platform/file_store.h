// SPDX-License-Identifier: MIT
#ifndef GM_PC_FILE_STORE_H
#define GM_PC_FILE_STORE_H

#include <map>
#include <mutex>
#include <string>

#include "app/config.h"

namespace gmpc {

// Configuration file for the PC build (NVS equivalent). Format: one
// "key=value" per line, '\' escapes for newline/backslash. Written
// atomically (temp file + rename). Keep it out of the repository: it
// contains the Wi-Fi password and write keys (see .gitignore).
class FileStore : public gm::KeyValueStore {
 public:
  explicit FileStore(const std::string& path);

  bool load();  // false if the file exists but cannot be read
  bool get(const char* key, char* out, size_t cap) override;
  bool set(const char* key, const char* value) override;
  bool commit() override;
  const std::string& path() const { return path_; }

 private:
  std::mutex mu_;
  std::string path_;
  std::map<std::string, std::string> values_;
};

}  // namespace gmpc

#endif
