// SPDX-License-Identifier: MIT
#include "util/log_buffer.h"

#include <string.h>

namespace gm {

void LogBuffer::finish() {
  // Truncation must not leave half a UTF-8 character in the JSON string.
  size_t lead = partialLen_;
  while (lead &&
         (static_cast<unsigned char>(partial_[lead - 1]) & 0xc0) == 0x80)
    --lead;
  if (lead) {
    --lead;
    const unsigned char c = static_cast<unsigned char>(partial_[lead]);
    const size_t width = c >= 0xc2 && c <= 0xdf   ? 2
                         : c >= 0xe0 && c <= 0xef ? 3
                         : c >= 0xf0 && c <= 0xf4 ? 4
                                                  : 1;
    if (partialLen_ - lead < width) partialLen_ = lead;
  }
  partial_[partialLen_] = '\0';
  if (count_ == 16) {
    head_ = (head_ + 1) % 16;
    --count_;
  }
  memcpy(lines_[(head_ + count_) % 16], partial_, partialLen_ + 1);
  ++count_;
  ++last_;
  partialLen_ = 0;
}

void LogBuffer::append(const char* text, size_t len, bool truncated) {
  for (size_t i = 0; i < len; ++i) {
    if (text[i] == '\n') {
      finish();
    } else if (text[i] && partialLen_ < sizeof(partial_) - 1) {
      partial_[partialLen_++] = text[i];
    }
  }
  if (truncated && partialLen_) finish();
}

bool LogBuffer::read(uint64_t since, char* out, size_t cap) {
  if (cap > 8192) cap = 8192;
  if (cap < 96) return false;
  const uint64_t oldest = last_ - count_;
  const uint64_t lost = since < oldest ? oldest - since : 0;
  uint64_t next = since > last_ ? last_ : (since < oldest ? oldest : since);
  const size_t start = static_cast<size_t>(next - oldest);
  size_t end = start, bytes = 96;
  for (; end < count_; ++end) {
    const char* line = lines_[(head_ + end) % 16];
    size_t cost = 3;  // quotes and comma
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(line);
         *p; ++p) {
      cost += *p < 32 && *p != '\n' && *p != '\r' && *p != '\t'
                  ? 6
                  : (*p < 32 || *p == '"' || *p == '\\' ? 2 : 1);
    }
    if (bytes + cost >= cap) break;
    bytes += cost;
  }
  if (end == start && start < count_) return false;
  next += end - start;
  JsonWriter w(out, cap);
  w.beginObject().kvu("next", next).kvu("lost", lost).key("lines").beginArray();
  for (size_t i = start; i < end; ++i) w.str(lines_[(head_ + i) % 16]);
  w.endArray().endObject();
  return w.ok();
}

}  // namespace gm
