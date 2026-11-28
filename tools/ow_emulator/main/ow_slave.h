// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

// The host build has no dependency on ESP-IDF.
#ifdef OW_SLAVE_EMBEDDED
#define OW_IRAM __attribute__((section(".iram1"), noinline))
#else
#define OW_IRAM
#endif

namespace ow {
constexpr size_t kMaxDevices = 8;
constexpr size_t kLogSize = 200;
constexpr uint32_t kResetUs = 400;
constexpr uint32_t kSampleUs = 30;

struct DeviceConfig {
  bool enabled = true;
  uint8_t rom[8] = {};
  int16_t temperature = 320;
  bool realSource = false;
  uint32_t conversionMs = 750;
  bool powered = true;
  bool powerOn85 = true;
  uint8_t th = 75;
  uint8_t tl = 70;
  uint8_t config = 0x7f;
  uint8_t reserved[3] = {0xff, 0x0c, 0x10};
  bool rawEnabled = false;
  uint8_t raw[8] = {};
};

struct Config {
  uint8_t count = 2;
  DeviceConfig devices[kMaxDevices];
};

struct Transaction {
  uint64_t timeUs = 0;
  uint8_t romCommand = 0;
  uint8_t rom[8] = {};
  uint8_t function = 0;
  uint8_t selected = 0;
  uint16_t readBits = 0;
  bool invalidCommand = false;
};

struct Counters {
  uint32_t resets = 0;
  uint32_t invalidCommands = 0;
  uint32_t unmatchedRoms = 0;
  uint32_t shortPulses = 0;
};

OW_IRAM uint8_t crc8(const uint8_t* data, size_t length);
Config defaultConfig();

class Slave {
 public:
  OW_IRAM void configure(const Config& config);
  OW_IRAM void setTemperature(size_t device, int16_t temperature);
  // nowUs is monotonic, independent of CPU frequency/cycle-counter wrap.
  OW_IRAM bool lowStarted(uint64_t nowUs);  // true = pull low (read zero)
  OW_IRAM bool lowEnded(uint32_t widthUs, uint64_t nowUs);  // reset => presence
  OW_IRAM bool bitToSend(uint64_t nowUs);
  OW_IRAM void idle();  // finish log, but preserve protocol across idle gaps
  const Counters& counters() const { return counters_; }
  size_t logCount() const { return logCount_; }
  uint32_t logSequence() const { return logSequence_; }
  const Transaction& logAt(size_t oldestIndex) const {
    return logs_[(logHead_ + kLogSize - logCount_ + oldestIndex) % kLogSize];
  }

 private:
  enum class State : uint8_t {
    Rom,
    Match,
    Function,
    Write,
    Search,
    ReadRom,
    ReadScratch,
    Conversion,
    Power,
    Recall,
    Silent
  };
  struct Device {
    DeviceConfig settings;
    uint8_t scratch[9] = {};
    uint8_t saved[3] = {};
    bool converting = false;
    uint64_t readyUs = 0;
    bool alarm = false;
  };
  OW_IRAM void reset(uint64_t nowUs);
  OW_IRAM void receive(bool bit, uint64_t nowUs);
  OW_IRAM void byteReceived(uint8_t byte, uint64_t nowUs);
  OW_IRAM void advanceRead();
  OW_IRAM void updateConversions(uint64_t nowUs);
  OW_IRAM void makeScratch(Device& device, bool startup);
  OW_IRAM void finishLog();
  OW_IRAM uint8_t enabledMask() const;
  OW_IRAM bool isReading() const;
  OW_IRAM void beginLog(uint64_t nowUs);

  Device devices_[kMaxDevices];
  uint8_t count_ = 0;
  State state_ = State::Silent;
  uint8_t selected_ = 0;
  uint8_t active_ = 0;
  uint8_t searchBit_ = 0;
  uint8_t searchPhase_ = 0;
  uint8_t input_ = 0;
  uint8_t inputBits_ = 0;
  uint8_t byteIndex_ = 0;
  uint16_t readBit_ = 0;
  uint8_t match_[8] = {};
  bool slotReading_ = false;
  bool slotStarted_ = false;
  bool logOpen_ = false;
  uint64_t transactionTime_ = 0;
  Transaction current_;
  Transaction logs_[kLogSize];
  size_t logHead_ = 0;
  size_t logCount_ = 0;
  uint32_t logSequence_ = 0;
  Counters counters_;
};
}  // namespace ow
