// SPDX-License-Identifier: MIT
#include "sim/sim_probe.h"

#include <cmath>

#include "onewire/ds18b20.h"
#include "util/crc.h"

namespace gmpc {

gm::Rom SimProbe::makeRom(uint8_t family, uint32_t serial) {
  gm::Rom rom = {};
  rom.b[0] = family;
  rom.b[1] = static_cast<uint8_t>(serial);
  rom.b[2] = static_cast<uint8_t>(serial >> 8);
  rom.b[3] = static_cast<uint8_t>(serial >> 16);
  rom.b[4] = static_cast<uint8_t>(serial >> 24);
  rom.b[5] = 0x16;
  rom.b[6] = 0x03;
  rom.b[7] = gm::crc8Maxim(rom.b, 7);
  return rom;
}

SimProbe::SimProbe(SimClock& clock, int sensors)
    : clock_(clock),
      mode_(Mode::Idle),
      bitCount_(0),
      byte_(0),
      searchBit_(0),
      searchPhase_(0),
      matchBit_(0),
      convertStartMs_(0),
      readBitPos_(0),
      shorted_(false),
      unsupported_(false) {
  // Serial numbers chosen so that the ROM order differs from the point
  // order (as on a real probe) and the search has to branch.
  static const uint32_t kSerials[] = {0x1E8564FF, 0x0A11C233, 0x7F0001A4,
                                      0x33B2C1D0, 0x5D9E0042, 0x00A1B2C3,
                                      0x6C6C1F1F, 0x2BADF00D};
  for (int i = 0; i < sensors && i < 8; ++i) {
    Device d;
    d.rom = makeRom(0x28, kSerials[i]);
    d.present = true;
    d.raw = 0;
    d.tempC = 12.0 + 0.6 * i;
    d.converted = false;
    d.crcFaults = 0;
    d.fixedTemp = false;
    devices_.push_back(d);
  }
}

size_t SimProbe::deviceCount() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return devices_.size();
}

bool SimProbe::romBit(const Device& d, int bit) const {
  return (d.rom.b[bit / 8] >> (bit % 8)) & 1;
}

void SimProbe::updateTemperatures() {
  // Slow daily-like swing so the portal shows changing values.
  const double hours = static_cast<double>(clock_.monoMs()) / 3600000.0;
  for (size_t i = 0; i < devices_.size(); ++i) {
    Device& d = devices_[i];
    if (!d.fixedTemp) {
      d.tempC = 12.0 + 0.6 * static_cast<double>(i) +
                1.5 * std::sin(hours / 24.0 * 6.2831853 + 0.3 * i);
    }
  }
}

bool SimProbe::reset() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (shorted_) {
    mode_ = Mode::Idle;
    return true;  // line held low looks like a (very long) presence pulse
  }
  bool any = false;
  active_.assign(devices_.size(), false);
  for (size_t i = 0; i < devices_.size(); ++i) {
    active_[i] = devices_[i].present;
    any = any || devices_[i].present;
  }
  mode_ = any ? Mode::RomCmd : Mode::Idle;
  bitCount_ = 0;
  byte_ = 0;
  return any;
}

void SimProbe::onByte(uint8_t value) {
  if (mode_ == Mode::RomCmd) {
    switch (value) {
      case gm::kCmdSearchRom:
        mode_ = Mode::Search;
        searchBit_ = 0;
        searchPhase_ = 0;
        return;
      case gm::kCmdMatchRom:
        mode_ = Mode::Match;
        matchBit_ = 0;
        return;
      case gm::kCmdSkipRom:
        mode_ = Mode::FuncCmd;
        return;
      default:
        mode_ = Mode::Idle;
        return;
    }
  }
  if (mode_ == Mode::FuncCmd) {
    if (value == gm::kCmdConvertT) {
      updateTemperatures();
      convertStartMs_ = clock_.monoMs();
      for (size_t i = 0; i < devices_.size(); ++i) {
        if (active_[i]) {
          Device& d = devices_[i];
          d.raw = static_cast<int16_t>(std::lround(d.tempC * 16.0));
          d.converted = true;
        }
      }
      mode_ = Mode::Converting;
    } else if (value == gm::kCmdReadScratchpad) {
      scratch_.assign(devices_.size(), std::vector<uint8_t>(9, 0xFF));
      for (size_t i = 0; i < devices_.size(); ++i) {
        if (active_[i]) {
          buildScratchpad(devices_[i], scratch_[i].data());
        }
      }
      readBitPos_ = 0;
      mode_ = Mode::ReadScratch;
    } else {
      mode_ = Mode::Idle;
    }
  }
}

void SimProbe::buildScratchpad(Device& d, uint8_t sp[9]) {
  const int16_t raw = d.converted ? d.raw : static_cast<int16_t>(0x0550);
  sp[0] = static_cast<uint8_t>(raw & 0xFF);
  sp[1] = static_cast<uint8_t>((raw >> 8) & 0xFF);
  sp[2] = d.th;
  sp[3] = d.tl;
  sp[4] = 0x7F;  // 12-bit resolution
  sp[5] = 0xFF;
  sp[6] = 0x0C;
  sp[7] = 0x10;
  sp[8] = gm::crc8Maxim(sp, 8);
  if (d.crcFaults != 0) {
    sp[1] ^= 0x04;  // bit flip on the line -> CRC mismatch
    if (d.crcFaults > 0) {
      --d.crcFaults;
    }
  }
}

void SimProbe::writeBit(bool bit) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  switch (mode_) {
    case Mode::RomCmd:
    case Mode::FuncCmd:
      if (bit) {
        byte_ = static_cast<uint8_t>(byte_ | (1u << bitCount_));
      }
      if (++bitCount_ == 8) {
        const uint8_t v = byte_;
        bitCount_ = 0;
        byte_ = 0;
        onByte(v);
      }
      break;
    case Mode::Search:
      if (searchPhase_ == 2) {
        for (size_t i = 0; i < devices_.size(); ++i) {
          if (active_[i] && romBit(devices_[i], searchBit_) != bit) {
            active_[i] = false;
          }
        }
        searchPhase_ = 0;
        if (++searchBit_ == 64) {
          mode_ = Mode::FuncCmd;  // selected device now addressed
          bitCount_ = 0;
          byte_ = 0;
        }
      }
      break;
    case Mode::Match:
      for (size_t i = 0; i < devices_.size(); ++i) {
        if (active_[i] && romBit(devices_[i], matchBit_) != bit) {
          active_[i] = false;
        }
      }
      if (++matchBit_ == 64) {
        mode_ = Mode::FuncCmd;
        bitCount_ = 0;
        byte_ = 0;
      }
      break;
    default:
      break;
  }
}

bool SimProbe::readBit() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (shorted_) {
    return false;
  }
  switch (mode_) {
    case Mode::Search: {
      if (searchPhase_ > 1) {
        return true;
      }
      bool value = true;  // wired AND, pull-up when nobody drives
      for (size_t i = 0; i < devices_.size(); ++i) {
        if (active_[i]) {
          const bool b = romBit(devices_[i], searchBit_);
          value = value && (searchPhase_ == 0 ? b : !b);
        }
      }
      ++searchPhase_;
      return value;
    }
    case Mode::ReadScratch: {
      if (readBitPos_ >= 72) {
        return true;
      }
      bool value = true;
      for (size_t i = 0; i < devices_.size(); ++i) {
        if (active_[i]) {
          value = value &&
                  ((scratch_[i][readBitPos_ / 8] >> (readBitPos_ % 8)) & 1);
        }
      }
      ++readBitPos_;
      return value;
    }
    case Mode::Converting:
      return clock_.monoMs() - convertStartMs_ >= gm::kConversionTimeMs12Bit;
    default:
      return true;
  }
}

void SimProbe::sleepMs(uint32_t ms) { clock_.sleepMs(ms); }

bool SimProbe::injectCrc(int point, int count) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (point < 1 || point > static_cast<int>(devices_.size())) {
    return false;
  }
  devices_[static_cast<size_t>(point - 1)].crcFaults = count;
  return true;
}

bool SimProbe::setPresent(int point, bool present) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (point < 1 || point > static_cast<int>(devices_.size())) {
    return false;
  }
  devices_[static_cast<size_t>(point - 1)].present = present;
  return true;
}

bool SimProbe::setTemperature(int point, double tempC) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (point < 1 || point > static_cast<int>(devices_.size()) || tempC < -55 ||
      tempC > 125) {
    return false;
  }
  Device& d = devices_[static_cast<size_t>(point - 1)];
  d.tempC = tempC;
  d.fixedTemp = true;
  return true;
}

bool SimProbe::setPointNumber(int index, uint8_t th, uint8_t tl) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (index < 1 || index > static_cast<int>(devices_.size())) {
    return false;
  }
  Device& d = devices_[static_cast<size_t>(index - 1)];
  d.th = th;
  d.tl = tl;
  return true;
}

void SimProbe::setUnsupportedDevice(bool on) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (on == unsupported_) {
    return;
  }
  unsupported_ = on;
  if (on) {
    Device d;
    d.rom = makeRom(0x10, 0x00C0FFEE);  // DS18S20 / DS1820
    d.present = true;
    d.raw = 0;
    d.tempC = 15.0;
    d.converted = false;
    d.crcFaults = 0;
    d.fixedTemp = true;
    devices_.push_back(d);
  } else {
    for (size_t i = 0; i < devices_.size(); ++i) {
      if (devices_[i].rom.family() == 0x10) {
        devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
  }
}

void SimProbe::setShorted(bool on) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  shorted_ = on;
}

void SimProbe::writeJson(gm::JsonWriter& w) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  w.key("probe")
      .beginObject()
      .kvb("shorted", shorted_)
      .key("devices")
      .beginArray();
  for (size_t i = 0; i < devices_.size(); ++i) {
    const Device& d = devices_[i];
    char hex[gm::kRomTextLen + 1];
    w.beginObject()
        .kvu("index", i + 1)
        .kv("rom", gm::romToHex(d.rom, hex, sizeof(hex)))
        .kvb("present", d.present)
        .kv("crc_faults", static_cast<int64_t>(d.crcFaults))
        .key("temp_c")
        .centi(static_cast<int32_t>(std::lround(d.tempC * 100.0)))
        .endObject();
  }
  w.endArray().endObject();
}

}  // namespace gmpc
