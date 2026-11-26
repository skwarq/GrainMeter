// SPDX-License-Identifier: MIT
#ifndef GM_LOG_BUFFER_H
#define GM_LOG_BUFFER_H

#include "app/platform.h"

namespace gm {

// Caller serializes append/read. Storage is allocated by the platform, not
// by the hook. Sequences identify complete lines; next is the last delivered.
class LogBuffer : public LogSource {
 public:
  explicit LogBuffer(uint64_t last = 0)
      : last_(last), head_(0), count_(0), partialLen_(0) {}
  void append(const char* text, size_t len, bool truncated = false);
  bool read(uint64_t since, char* out, size_t cap) override;
  uint64_t last() const { return last_; }

 private:
  void finish();
  uint64_t last_;
  size_t head_, count_, partialLen_;
  char lines_[16][256];
  char partial_[256];
};

}  // namespace gm
#endif
