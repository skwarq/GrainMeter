// SPDX-License-Identifier: MIT
#include "ow_decoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ow {

uint8_t crc8(const uint8_t* data, size_t length) {
  uint8_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    uint8_t value = data[i];
    for (unsigned j = 0; j < 8; ++j) {
      const bool mix = (crc ^ value) & 1;
      crc >>= 1;
      if (mix) crc ^= 0x8c;
      value >>= 1;
    }
  }
  return crc;
}

Decoder::Decoder(Sink sink, void* context, double offset_us)
    : sink_(sink), context_(context), time_(offset_us) {}

bool Decoder::feed(Record record) {
  if (record.level > 1 || !std::isfinite(record.duration_us) ||
      record.duration_us <= 0)
    return false;
  if (pending_count_ == 0) {
    pending_[pending_count_++] = record;
  } else if (pending_[pending_count_ - 1].level == record.level) {
    pending_[pending_count_ - 1].duration_us += record.duration_us;
  } else if (pending_count_ == 1) {
    pending_[pending_count_++] = record;
  } else if (pending_[1].duration_us < 3) {
    pending_[0].duration_us += pending_[1].duration_us + record.duration_us;
    pending_count_ = 1;
  } else {
    if (!consume(pending_[0])) return false;
    pending_[0] = pending_[1];
    pending_[1] = record;
  }
  return true;
}

bool Decoder::finish() {
  if (pending_count_ == 2 && pending_[1].duration_us < 3) {
    pending_[0].duration_us += pending_[1].duration_us;
    pending_count_ = 1;
  }
  for (size_t i = 0; i < pending_count_; ++i) {
    if (!consume(pending_[i])) return false;
  }
  pending_count_ = 0;
  return true;
}

bool Decoder::consume(Record record) {
  const double at = time_;
  time_ += record.duration_us;
  if (record.level || record.duration_us < 3) return true;
  if (record.duration_us > 400) {
    if (!emit()) return false;
    active_ = true;
    presence_window_ = true;
    presence_ = invalid_ = truncated_ = search_ = false;
    start_ = at;
    reset_end_ = time_;
    byte_count_ = byte_bits_ = search_bits_ = 0;
    byte_ = 0;
    std::memset(rom_, 0, sizeof(rom_));
    return true;
  }
  if (!active_) return true;
  if (presence_window_) {
    presence_window_ = false;
    if (at - reset_end_ < 70 && record.duration_us >= 50 &&
        record.duration_us <= 300) {
      presence_ = true;
      presence_us_ = record.duration_us;
      return true;
    }
  }
  if (record.duration_us > 300) {
    invalid_ = true;
    return true;
  }
  bit(record.duration_us < 24);
  return true;
}

void Decoder::bit(bool value) {
  if (search_) {
    if (search_bits_ < 192 && search_bits_ % 3 == 2 && value) {
      const size_t index = search_bits_ / 3;
      rom_[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
    }
    ++search_bits_;
    if (search_bits_ > 192) invalid_ = true;
    return;
  }
  if (value) byte_ |= static_cast<uint8_t>(1u << byte_bits_);
  if (++byte_bits_ != 8) return;
  if (byte_count_ < sizeof(bytes_)) {
    bytes_[byte_count_++] = byte_;
    if (byte_count_ == 1 && byte_ == 0xf0) search_ = true;
  } else {
    truncated_ = true;
  }
  byte_ = 0;
  byte_bits_ = 0;
}

bool Decoder::emit() {
  if (!active_) return true;
  active_ = false;
  char line[1024];
  size_t length = static_cast<size_t>(
      std::snprintf(line, sizeof(line), "%.4fs RST pres=", start_ / 1000000));
  auto append = [&](const char* format, auto... args) {
    const int n =
        std::snprintf(line + length, sizeof(line) - length, format, args...);
    if (n > 0) length += static_cast<size_t>(n);
  };
  if (presence_) {
    append("Y(%.0fus)", presence_us_);
  } else {
    append("%s", "N");
  }
  if (search_) {
    append("%s", " F0 SEARCH ROM=");
    for (size_t i = 0; i < 8; ++i) append("%s%02x", i ? " " : "", rom_[i]);
    append(" (%zu bits)", search_bits_);
    if (search_bits_ == 192)
      append(" crc=%s", crc8(rom_, 8) == 0 ? "ok" : "BAD");
  } else {
    for (size_t i = 0; i < byte_count_; ++i) append(" %02X", bytes_[i]);
    if (byte_bits_) append(" partial=%ubits", byte_bits_);
    size_t command = sizeof(bytes_);
    uint8_t family = 0x28;
    if (byte_count_ >= 10 && bytes_[0] == 0x55) {
      command = 9;
      family = bytes_[1];
    } else if (byte_count_ >= 2 && bytes_[0] == 0xcc) {
      command = 1;
    }
    if (command < byte_count_ && bytes_[command] == 0xbe &&
        byte_count_ >= command + 3 &&
        (family == 0x28 || family == 0x10 || family == 0)) {
      const uint8_t* scratch = bytes_ + command + 1;
      const unsigned raw =
          scratch[0] | (static_cast<unsigned>(scratch[1]) << 8);
      const int signed_raw =
          raw >= 0x8000 ? static_cast<int>(raw) - 65536 : static_cast<int>(raw);
      append(" T=%.4fC", signed_raw / (family == 0x10 ? 2.0 : 16.0));
      if (byte_count_ >= command + 10)
        append(" crc=%s", crc8(scratch, 9) == 0 ? "ok" : "BAD");
    }
  }
  if (invalid_) append("%s", " invalid=Y");
  if (truncated_) append("%s", " truncated=Y");
  append("%s", "\n");
  return sink_(context_, line);
}

}  // namespace ow
