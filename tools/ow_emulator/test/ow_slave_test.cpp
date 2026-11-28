// SPDX-License-Identifier: MIT
#include "ow_slave.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                        \
    }                                                                      \
  } while (0)

using Rom = std::array<uint8_t, 8>;
using Scratch = std::array<uint8_t, 9>;

struct Master {
  ow::Slave slave;
  uint64_t now = 0;
  explicit Master(const ow::Config& config) { slave.configure(config); }
  bool reset() {
    slave.lowStarted(now);
    now += 490;
    const bool presence = slave.lowEnded(490, now);
    now += 300;
    return presence;
  }
  void writeBit(bool bit, unsigned zeroWidth = 78) {
    CHECK(!slave.lowStarted(now));
    const unsigned width = bit ? 17 : zeroWidth;
    slave.lowEnded(width, now + width);
    now += width > 70 ? width + 5 : 70;
  }
  bool readBit() {
    const bool pull = slave.lowStarted(now);
    // Slave holds a zero until 30 us; master releases after 12 us.
    slave.lowEnded(pull ? 30 : 12, now + (pull ? 30 : 12));
    now += 70;
    return !pull;
  }
  void write(uint8_t byte) {
    for (unsigned bit = 0; bit < 8; ++bit) writeBit((byte >> bit) & 1);
  }
  uint8_t read() {
    uint8_t byte = 0;
    for (unsigned bit = 0; bit < 8; ++bit)
      if (readBit()) byte |= 1u << bit;
    return byte;
  }
  void match(const uint8_t* rom, uint8_t function) {
    CHECK(reset());
    write(0x55);
    for (unsigned i = 0; i < 8; ++i) write(rom[i]);
    write(function);
  }
  Scratch scratch(const uint8_t* rom) {
    match(rom, 0xbe);
    Scratch result;
    for (auto& byte : result) byte = read();
    return result;
  }
  std::set<Rom> search(uint8_t command = 0xf0) {
    std::set<Rom> found;
    Rom previous{};
    unsigned lastDiscrepancy = 0;
    bool last = false;
    for (unsigned attempt = 0; attempt < 16 && !last; ++attempt) {
      CHECK(reset());
      write(command);
      Rom rom{};
      unsigned discrepancy = 0;
      for (unsigned position = 1; position <= 64; ++position) {
        const bool bit = readBit();
        const bool inverse = readBit();
        if (bit && inverse) return found;
        bool direction = bit;
        if (!bit && !inverse) {
          direction =
              position < lastDiscrepancy
                  ? (previous[(position - 1) / 8] >> ((position - 1) % 8)) & 1
                  : position == lastDiscrepancy;
          if (!direction) discrepancy = position;
        }
        if (direction) rom[(position - 1) / 8] |= 1u << ((position - 1) % 8);
        writeBit(direction);
      }
      CHECK(found.insert(rom).second);
      previous = rom;
      lastDiscrepancy = discrepancy;
      last = discrepancy == 0;
    }
    CHECK(last);
    return found;
  }
};

static Rom romOf(const ow::DeviceConfig& config) {
  Rom rom;
  for (size_t i = 0; i < 8; ++i) rom[i] = config.rom[i];
  return rom;
}

static void checkCrc(const Scratch& scratch) {
  CHECK(ow::crc8(scratch.data(), 8) == scratch[8]);
}

int main() {
  auto config = ow::defaultConfig();
  Master master(config);
  CHECK(master.search() ==
        (std::set<Rom>{romOf(config.devices[0]), romOf(config.devices[1])}));
  auto scratch = master.scratch(config.devices[0].rom);
  CHECK(scratch[0] == 0x50 && scratch[1] == 0x05);
  CHECK(scratch[2] == 0x4b && scratch[3] == 0x46 && scratch[4] == 0x7f);
  CHECK(scratch[5] == 0xff && scratch[6] == 0x0c && scratch[7] == 0x10);
  CHECK(scratch[8] == 0x1c);  // Known Maxim power-up scratchpad vector.
  checkCrc(scratch);
  master.match(config.devices[0].rom, 0x44);
  CHECK(!master.readBit());
  master.slave.idle();
  master.now += 749000;
  CHECK(!master.readBit());
  master.now += 1000;
  CHECK(master.readBit());
  scratch = master.scratch(config.devices[0].rom);
  CHECK(scratch[0] == 0x40 && scratch[1] == 0x01);
  checkCrc(scratch);
  // STW reads only eight bytes, then resets without ever reading CRC.
  master.match(config.devices[1].rom, 0xbe);
  for (unsigned i = 0; i < 8; ++i) master.read();
  scratch = master.scratch(config.devices[0].rom);
  CHECK(scratch[0] == 0x40);
  const uint8_t zeroRom[8] = {};
  master.match(zeroRom, 0xbe);
  for (unsigned i = 0; i < 9; ++i) CHECK(master.read() == 0xff);
  CHECK(master.slave.counters().unmatchedRoms == 1);
  master.slave.idle();
  const auto& log = master.slave.logAt(master.slave.logCount() - 1);
  CHECK(log.romCommand == 0x55 && log.function == 0xbe && log.readBits == 72);

  config.count = 1;
  config.devices[0].powerOn85 = false;
  config.devices[0].temperature = -163;
  config.devices[0].config = 0x1f;
  Master single(config);
  CHECK(single.reset());
  single.write(0xcc);
  single.write(0xbe);
  for (auto& byte : scratch) byte = single.read();
  CHECK(scratch[0] == 0x58 && scratch[1] == 0xff);  // -10.5, nine bits
  checkCrc(scratch);
  CHECK(single.reset());
  single.write(0x33);
  for (size_t i = 0; i < 8; ++i)
    CHECK(single.read() == config.devices[0].rom[i]);
  single.match(config.devices[0].rom, 0x4e);
  single.write(0x20);
  single.write(0xf0);
  single.write(0x60);
  single.match(config.devices[0].rom, 0x48);
  CHECK(single.readBit());
  single.match(config.devices[0].rom, 0x4e);
  single.write(0x10);
  single.write(0x01);
  single.write(0x00);
  single.match(config.devices[0].rom, 0xb8);
  CHECK(single.readBit());
  scratch = single.scratch(config.devices[0].rom);
  CHECK(scratch[2] == 0x20 && scratch[3] == 0xf0 && scratch[4] == 0x7f);
  checkCrc(scratch);
  single.match(config.devices[0].rom, 0xb4);
  CHECK(single.readBit());

  config.devices[0].powered = false;
  config.devices[0].conversionMs = 0;
  config.devices[0].temperature = 800;
  config.devices[0].th = 30;
  Master alarm(config);
  CHECK(alarm.search(0xec).empty());
  alarm.match(config.devices[0].rom, 0x44);
  CHECK(alarm.readBit());
  CHECK(alarm.search(0xec) == std::set<Rom>{romOf(config.devices[0])});
  alarm.match(config.devices[0].rom, 0xb4);
  CHECK(!alarm.readBit());
  config.devices[0].enabled = false;
  Master empty(config);
  CHECK(!empty.reset());
  empty.write(0x33);
  CHECK(empty.read() == 0xff);

  // Eight arbitrary ROMs, including a deliberately invalid all-zero ROM.
  config.count = 8;
  for (size_t i = 0; i < 8; ++i) {
    config.devices[i].enabled = i != 3;
    for (size_t j = 0; j < 8; ++j)
      config.devices[i].rom[j] = i ? static_cast<uint8_t>(i * 17 + j) : 0;
    config.devices[i].powerOn85 = false;
    config.devices[i].conversionMs = static_cast<uint32_t>(i * 10);
  }
  Master many(config);
  std::set<Rom> expected;
  for (const auto& device : config.devices)
    if (device.enabled) expected.insert(romOf(device));
  CHECK(many.search() == expected);
  many.match(config.devices[0].rom, 0xbe);
  many.readBit();
  CHECK(many.reset());  // abort even in the middle of a byte
  many.write(0xcc);
  many.write(0x44);
  CHECK(!many.readBit());  // AND of busy status
  many.now += 71000;
  CHECK(many.readBit());
  CHECK(many.reset());
  many.write(0x33);
  for (size_t j = 0; j < 8; ++j) {
    uint8_t wire = 0xff;
    for (size_t i = 0; i < 8; ++i)
      if (config.devices[i].enabled) wire &= config.devices[i].rom[j];
    CHECK(many.read() == wire);
  }

  config.count = 1;
  config.devices[0].rawEnabled = true;
  const uint8_t raw[8] = {0x50, 5, 1, 2, 3, 4, 5, 6};
  for (size_t i = 0; i < 8; ++i) config.devices[0].raw[i] = raw[i];
  Master rawMaster(config);
  scratch = rawMaster.scratch(config.devices[0].rom);
  for (size_t i = 0; i < 8; ++i) CHECK(scratch[i] == raw[i]);
  checkCrc(scratch);
  // Long write-zero pulses and bounded transaction ring.
  for (size_t i = 0; i < 210; ++i) {
    CHECK(rawMaster.reset());
    for (unsigned bit = 0; bit < 8; ++bit)
      rawMaster.writeBit((0xcc >> bit) & 1, 117);
    rawMaster.write(0xb4);
    CHECK(!rawMaster.readBit());
    rawMaster.slave.idle();
  }
  CHECK(rawMaster.slave.logCount() == 200);
  std::puts("DS18B20 pulse protocol: all tests passed");
}
