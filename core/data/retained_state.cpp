// SPDX-License-Identifier: MIT
#include "data/retained_state.h"

#include <string.h>

#include "util/crc.h"

namespace gm {

void RetainedState::init() {
  memset(this, 0, sizeof(*this));
  magic = kRetainedMagic;
  size = static_cast<uint32_t>(sizeof(*this));
  nextSeq = 1;
  last.httpStatus = -1;
  seal();
}

bool RetainedState::valid() const {
  if (magic != kRetainedMagic || size != sizeof(*this)) {
    return false;
  }
  if (head >= kBufferCapacity || count > kBufferCapacity) {
    return false;
  }
  const size_t len = offsetof(RetainedState, crc);
  return crc32(this, len) == crc;
}

void RetainedState::seal() { crc = crc32(this, offsetof(RetainedState, crc)); }

Sample* RetainedState::push(const Sample& s) {
  if (count == kBufferCapacity) {
    head = static_cast<uint16_t>((head + 1) % kBufferCapacity);
    --count;
    ++droppedOverflow;
  }
  const size_t idx = (head + count) % kBufferCapacity;
  items[idx] = s;
  items[idx].seq = nextSeq++;
  ++count;
  return &items[idx];
}

size_t RetainedState::removeUpTo(uint32_t lastSeq) {
  size_t removed = 0;
  while (count > 0 && items[head].seq <= lastSeq) {
    head = static_cast<uint16_t>((head + 1) % kBufferCapacity);
    --count;
    ++removed;
  }
  return removed;
}

void RetainedState::removeAt(size_t i) {
  if (i >= count) {
    return;
  }
  for (size_t k = i; k + 1 < count; ++k) {
    items[(head + k) % kBufferCapacity] =
        items[(head + k + 1) % kBufferCapacity];
  }
  --count;
}

}  // namespace gm
