// SPDX-License-Identifier: MIT
#include "ow_slave.h"

#include <array>

namespace ow {
namespace {
constexpr std::array<uint8_t, 256> crcTable() {
  std::array<uint8_t, 256> table{};
  for (size_t i = 0; i < table.size(); ++i) {
    uint8_t crc = static_cast<uint8_t>(i);
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0x8c : 0);
    table[i] = crc;
  }
  return table;
}
#ifdef OW_SLAVE_EMBEDDED
__attribute__((section(".dram1")))
#endif
const std::array<uint8_t, 256>
    kCrcTable = crcTable();
}  // namespace

uint8_t crc8(const uint8_t* data, size_t length) {
  uint8_t crc = 0;
  for (size_t i = 0; i < length; ++i) crc = kCrcTable[crc ^ data[i]];
  return crc;
}

Config defaultConfig() {
  Config config;
  const uint8_t roms[2][8] = {{0x28, 0xd0, 0x5b, 0xcf, 0x05, 0, 0, 0xa9},
                              {0x28, 0x6b, 0x40, 0xd0, 0x05, 0, 0, 0xf8}};
  for (size_t i = 0; i < 2; ++i) {
    for (size_t j = 0; j < 8; ++j) config.devices[i].rom[j] = roms[i][j];
  }
  config.devices[1].temperature = 344;
  return config;
}

void Slave::configure(const Config& config) {
  finishLog();
  count_ = config.count <= kMaxDevices ? config.count : kMaxDevices;
  for (size_t i = 0; i < count_; ++i) {
    devices_[i].settings = config.devices[i];
    devices_[i].converting = false;
    devices_[i].saved[0] = config.devices[i].th;
    devices_[i].saved[1] = config.devices[i].tl;
    devices_[i].saved[2] = config.devices[i].config;
    devices_[i].alarm = false;
    makeScratch(devices_[i], true);
  }
  state_ = State::Silent;
  selected_ = 0;
}

void Slave::setTemperature(size_t device, int16_t temperature) {
  if (device < count_) devices_[device].settings.temperature = temperature;
}

void Slave::makeScratch(Device& device, bool startup) {
  auto& cfg = device.settings;
  if (cfg.rawEnabled) {
    for (size_t j = 0; j < 8; ++j) device.scratch[j] = cfg.raw[j];
  } else {
    int16_t value = startup && cfg.powerOn85 ? 1360 : cfg.temperature;
    const unsigned resolution = (cfg.config >> 5) & 3;
    uint16_t bits = static_cast<uint16_t>(value);
    bits &= static_cast<uint16_t>(0xffffu << (3 - resolution));
    device.scratch[0] = static_cast<uint8_t>(bits);
    device.scratch[1] = static_cast<uint8_t>(bits >> 8);
    device.scratch[2] = cfg.th;
    device.scratch[3] = cfg.tl;
    device.scratch[4] = cfg.config;
    for (size_t j = 0; j < 3; ++j) device.scratch[5 + j] = cfg.reserved[j];
  }
  device.scratch[8] = crc8(device.scratch, 8);
}

uint8_t Slave::enabledMask() const {
  uint8_t mask = 0;
  for (size_t i = 0; i < count_; ++i)
    if (devices_[i].settings.enabled) mask |= 1u << i;
  return mask;
}

void Slave::beginLog(uint64_t nowUs) {
  current_ = Transaction{};
  current_.timeUs = nowUs;
  logOpen_ = true;
}

void Slave::finishLog() {
  if (!logOpen_) return;
  logs_[logHead_] = current_;
  logHead_ = (logHead_ + 1) % kLogSize;
  if (logCount_ < kLogSize) ++logCount_;
  ++logSequence_;
  logOpen_ = false;
}

void Slave::idle() { finishLog(); }

void Slave::updateConversions(uint64_t nowUs) {
  for (size_t i = 0; i < count_; ++i) {
    auto& device = devices_[i];
    if (!device.converting || nowUs < device.readyUs) continue;
    device.converting = false;
    makeScratch(device, false);
    const uint16_t bits = static_cast<uint16_t>(device.scratch[0]) |
                          (static_cast<uint16_t>(device.scratch[1]) << 8);
    const int16_t temperature = static_cast<int16_t>(bits);
    // Alarm comparison uses the signed integer part, as on the DS18B20.
    const int16_t whole =
        temperature >= 0 ? temperature / 16 : (temperature - 15) / 16;
    device.alarm = whole >= static_cast<int8_t>(device.scratch[2]) ||
                   whole <= static_cast<int8_t>(device.scratch[3]);
  }
}

void Slave::reset(uint64_t nowUs) {
  finishLog();
  updateConversions(nowUs);
  ++counters_.resets;
  state_ = enabledMask() ? State::Rom : State::Silent;
  selected_ = 0;
  active_ = 0;
  input_ = inputBits_ = byteIndex_ = searchBit_ = searchPhase_ = 0;
  readBit_ = 0;
  slotStarted_ = false;
  transactionTime_ = nowUs;
  beginLog(nowUs);
}

bool Slave::isReading() const {
  return state_ == State::ReadRom || state_ == State::ReadScratch ||
         state_ == State::Conversion || state_ == State::Power ||
         state_ == State::Recall ||
         (state_ == State::Search && searchPhase_ < 2);
}

bool Slave::bitToSend(uint64_t nowUs) {
  updateConversions(nowUs);
  bool value = true;
  for (size_t i = 0; i < count_; ++i) {
    const uint8_t mask = 1u << i;
    if (state_ == State::Search) {
      if (!(active_ & mask) || searchBit_ >= 64 || searchPhase_ >= 2) continue;
      bool bit =
          (devices_[i].settings.rom[searchBit_ / 8] >> (searchBit_ % 8)) & 1;
      value &= searchPhase_ == 0 ? bit : !bit;
    } else if (selected_ & mask) {
      if (state_ == State::ReadRom && readBit_ < 64) {
        value &= (devices_[i].settings.rom[readBit_ / 8] >> (readBit_ % 8)) & 1;
      } else if (state_ == State::ReadScratch && readBit_ < 72) {
        value &= (devices_[i].scratch[readBit_ / 8] >> (readBit_ % 8)) & 1;
      } else if (state_ == State::Conversion) {
        value &= !devices_[i].converting;
      } else if (state_ == State::Power) {
        value &= devices_[i].settings.powered;
      }
    }
  }
  return value;
}

bool Slave::lowStarted(uint64_t nowUs) {
  slotStarted_ = true;
  slotReading_ = isReading();
  return slotReading_ && !bitToSend(nowUs);
}

void Slave::advanceRead() {
  ++current_.readBits;
  if (state_ == State::Search) {
    ++searchPhase_;
  } else if (state_ == State::ReadRom || state_ == State::ReadScratch) {
    ++readBit_;
    if ((state_ == State::ReadRom && readBit_ == 64) ||
        (state_ == State::ReadScratch && readBit_ == 72))
      state_ = State::Silent;
  }
}

bool Slave::lowEnded(uint32_t widthUs, uint64_t nowUs) {
  if (widthUs >= kResetUs) {
    reset(nowUs);
    return enabledMask() != 0;
  }
  if (!slotStarted_) return false;
  slotStarted_ = false;
  if (!logOpen_ && state_ != State::Silent) {
    // An idle gap ends a log segment, not the master's ROM selection.
    current_.readBits = 0;
    logOpen_ = true;
  }
  if (widthUs < 2) {
    ++counters_.shortPulses;
    return false;
  }
  if (slotReading_)
    advanceRead();
  else
    receive(widthUs < kSampleUs, nowUs);
  return false;
}

void Slave::receive(bool bit, uint64_t nowUs) {
  if (state_ == State::Search) {
    if (bit) current_.rom[searchBit_ / 8] |= 1u << (searchBit_ % 8);
    for (size_t i = 0; i < count_; ++i) {
      const bool romBit =
          (devices_[i].settings.rom[searchBit_ / 8] >> (searchBit_ % 8)) & 1;
      if (romBit != bit) active_ &= ~(1u << i);
    }
    searchPhase_ = 0;
    if (++searchBit_ == 64) {
      selected_ = active_;
      current_.selected = selected_;
      state_ = State::Silent;
    }
    return;
  }
  if (state_ == State::Silent) return;
  if (bit) input_ |= 1u << inputBits_;
  if (++inputBits_ == 8) {
    const uint8_t byte = input_;
    input_ = inputBits_ = 0;
    byteReceived(byte, nowUs);
  }
}

void Slave::byteReceived(uint8_t byte, uint64_t nowUs) {
  if (state_ == State::Rom) {
    current_.romCommand = byte;
    if (byte == 0xf0 || byte == 0xec) {
      active_ = enabledMask();
      if (byte == 0xec) {
        updateConversions(nowUs);
        for (size_t i = 0; i < count_; ++i)
          if (!devices_[i].alarm) active_ &= ~(1u << i);
      }
      state_ = State::Search;
    } else if (byte == 0x55) {
      state_ = State::Match;
      byteIndex_ = 0;
    } else if (byte == 0xcc || byte == 0x33) {
      selected_ = enabledMask();
      current_.selected = selected_;
      readBit_ = 0;
      state_ = byte == 0xcc ? State::Function : State::ReadRom;
    } else {
      ++counters_.invalidCommands;
      current_.invalidCommand = true;
      state_ = State::Silent;
    }
  } else if (state_ == State::Match) {
    match_[byteIndex_] = byte;
    current_.rom[byteIndex_] = byte;
    if (++byteIndex_ == 8) {
      selected_ = enabledMask();
      for (size_t i = 0; i < count_; ++i)
        for (size_t j = 0; j < 8; ++j)
          if (devices_[i].settings.rom[j] != match_[j]) selected_ &= ~(1u << i);
      current_.selected = selected_;
      if (!selected_) ++counters_.unmatchedRoms;
      // Still decode the function for diagnostics, but never drive the line.
      state_ = State::Function;
    }
  } else if (state_ == State::Write) {
    for (size_t i = 0; i < count_; ++i) {
      if (!(selected_ & (1u << i))) continue;
      auto& device = devices_[i];
      if (byteIndex_ == 0) device.settings.th = byte;
      if (byteIndex_ == 1) device.settings.tl = byte;
      if (byteIndex_ == 2) device.settings.config = (byte & 0x60) | 0x1f;
      device.scratch[2 + byteIndex_] =
          byteIndex_ == 2 ? device.settings.config : byte;
      device.scratch[8] = crc8(device.scratch, 8);
    }
    if (++byteIndex_ == 3) state_ = State::Silent;
  } else if (state_ == State::Function) {
    current_.function = byte;
    readBit_ = byteIndex_ = 0;
    if (byte == 0xbe) {
      updateConversions(nowUs);
      state_ = State::ReadScratch;
    } else if (byte == 0x44) {
      for (size_t i = 0; i < count_; ++i) {
        if (!(selected_ & (1u << i))) continue;
        devices_[i].converting = true;
        devices_[i].readyUs =
            nowUs +
            static_cast<uint64_t>(devices_[i].settings.conversionMs) * 1000;
      }
      state_ = State::Conversion;
    } else if (byte == 0x4e) {
      state_ = State::Write;
    } else if (byte == 0xb4) {
      state_ = State::Power;
    } else if (byte == 0x48 || byte == 0xb8) {
      for (size_t i = 0; i < count_; ++i) {
        if (!(selected_ & (1u << i))) continue;
        auto& device = devices_[i];
        for (size_t j = 0; j < 3; ++j) {
          if (byte == 0x48)
            device.saved[j] = device.scratch[j + 2];
          else
            device.scratch[j + 2] = device.saved[j];
        }
        if (byte == 0xb8) {
          device.settings.th = device.saved[0];
          device.settings.tl = device.saved[1];
          device.settings.config = device.saved[2];
          device.scratch[8] = crc8(device.scratch, 8);
        }
      }
      // EEPROM is virtual: Copy/Recall complete immediately, read status = 1.
      state_ = State::Recall;
    } else {
      ++counters_.invalidCommands;
      current_.invalidCommand = true;
      state_ = State::Silent;
    }
  }
}
}  // namespace ow
