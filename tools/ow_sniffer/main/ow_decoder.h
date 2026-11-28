// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace ow {

struct Record {
  uint8_t level;
  double duration_us;
};

uint8_t crc8(const uint8_t* data, size_t length);

// The sink is synchronous; false stops decoding (e.g. a disconnected HTTP
// client).
class Decoder {
 public:
  using Sink = bool (*)(void*, const char*);
  Decoder(Sink sink, void* context, double offset_us = 0);
  bool feed(Record record);
  // Drain the glitch filter. Only the next reset closes a transaction; EOF does
  // not prove that the current transaction is complete.
  bool finish();

 private:
  bool consume(Record record);
  bool emit();
  void bit(bool value);
  Sink sink_;
  void* context_;
  double time_;
  Record pending_[2]{};
  size_t pending_count_ = 0;
  bool active_ = false;
  bool presence_window_ = false;
  bool presence_ = false;
  bool invalid_ = false;
  bool truncated_ = false;
  double start_ = 0;
  double reset_end_ = 0;
  double presence_us_ = 0;
  uint8_t bytes_[256]{};
  size_t byte_count_ = 0;
  uint8_t byte_ = 0;
  unsigned byte_bits_ = 0;
  bool search_ = false;
  size_t search_bits_ = 0;
  uint8_t rom_[8]{};
};

}  // namespace ow
