// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "onewire/ds18b20.h"
#include "sim/sim_probe.h"
#include "util/crc.h"

namespace {
// Builds a valid 12-bit scratchpad for a raw 1/16 degC value.
void makeScratchpad(int16_t raw, uint8_t sp[9], uint8_t config = 0x7F) {
  sp[0] = static_cast<uint8_t>(raw & 0xFF);
  sp[1] = static_cast<uint8_t>((raw >> 8) & 0xFF);
  sp[2] = 0x4B;
  sp[3] = 0x46;
  sp[4] = config;
  sp[5] = 0xFF;
  sp[6] = 0x0C;
  sp[7] = 0x10;
  sp[8] = gm::crc8Maxim(sp, 8);
}

int16_t decodeOk(int16_t raw, uint8_t config = 0x7F) {
  uint8_t sp[9];
  makeScratchpad(raw, sp, config);
  int16_t c = -9999;
  EXPECT_EQ(gm::decodeScratchpad(sp, &c), gm::ReadStatus::Ok);
  return c;
}
}  // namespace

// Values from the DS18B20 datasheet temperature/data relationship table.
TEST(Ds18b20Decode, DatasheetTable) {
  EXPECT_EQ(decodeOk(0x07D0), 12500);                        // +125
  EXPECT_EQ(decodeOk(0x0191), 2506);                         // +25.0625
  EXPECT_EQ(decodeOk(0x00A2), 1013);                         // +10.125
  EXPECT_EQ(decodeOk(0x0008), 50);                           // +0.5
  EXPECT_EQ(decodeOk(static_cast<int16_t>(0xFFF8)), -50);    // -0.5
  EXPECT_EQ(decodeOk(static_cast<int16_t>(0xFF5E)), -1013);  // -10.125
  EXPECT_EQ(decodeOk(static_cast<int16_t>(0xFE6F)), -2506);  // -25.0625
  EXPECT_EQ(decodeOk(static_cast<int16_t>(0xFC90)), -5500);  // -55
}

TEST(Ds18b20Decode, ZeroIsAValidReading) {
  uint8_t sp[9];
  makeScratchpad(0, sp);
  int16_t c = 1234;
  EXPECT_EQ(gm::decodeScratchpad(sp, &c), gm::ReadStatus::Ok);
  EXPECT_EQ(c, 0);
}

TEST(Ds18b20Decode, AllOnesMeansMissing) {
  uint8_t sp[9];
  for (auto& b : sp) b = 0xFF;
  int16_t c = 0;
  EXPECT_EQ(gm::decodeScratchpad(sp, &c), gm::ReadStatus::Missing);
}

TEST(Ds18b20Decode, AllZeroIsBusErrorNotZeroDegrees) {
  uint8_t sp[9] = {0};
  int16_t c = 77;
  EXPECT_EQ(gm::decodeScratchpad(sp, &c), gm::ReadStatus::BusError);
  EXPECT_EQ(c, 77);  // untouched
}

TEST(Ds18b20Decode, CrcError) {
  uint8_t sp[9];
  makeScratchpad(0x0191, sp);
  sp[0] ^= 0x01;
  int16_t c = 0;
  EXPECT_EQ(gm::decodeScratchpad(sp, &c), gm::ReadStatus::CrcError);
}

TEST(Ds18b20Decode, PowerOnValue) {
  const uint8_t sp[9] = {0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x1C};
  int16_t c = 0;
  EXPECT_EQ(gm::decodeScratchpad(sp, &c), gm::ReadStatus::PowerOnValue);
}

TEST(Ds18b20Decode, OutOfRange) {
  uint8_t sp[9];
  makeScratchpad(0x07E0, sp);  // 126 degC
  int16_t c = 0;
  EXPECT_EQ(gm::decodeScratchpad(sp, &c), gm::ReadStatus::OutOfRange);
}

TEST(Ds18b20Decode, LowerResolutionMasksUndefinedBits) {
  // 9-bit (config 0x1F): bits 2..0 undefined -> 0x0197 decodes as 0x0190
  EXPECT_EQ(decodeOk(0x0197, 0x1F), 2500);
  // 11-bit (config 0x5F): bit 0 undefined
  EXPECT_EQ(decodeOk(0x0191, 0x5F), 2500);
}

TEST(Ds18b20Bus, ReadsSimulatedSensorAfterConversion) {
  gmpc::SimClock clock(1.0, true);
  gmpc::SimProbe probe(clock, 2);
  probe.setTemperature(1, 0.0);
  probe.setTemperature(2, -3.5);
  const gm::Rom r1 = gmpc::SimProbe::makeRom(0x28, 0x1E8564FF);
  int16_t c = 0;
  // Before any conversion: power-on value
  EXPECT_EQ(gm::readSensor(probe, r1, &c), gm::ReadStatus::PowerOnValue);
  ASSERT_TRUE(gm::startConversionAll(probe));
  bool timeout = true;
  const uint32_t ms = gm::waitConversion(probe, 800, &timeout);
  EXPECT_FALSE(timeout);
  EXPECT_GE(ms, 740u);
  EXPECT_LE(ms, 760u);
  EXPECT_EQ(gm::readSensor(probe, r1, &c), gm::ReadStatus::Ok);
  EXPECT_EQ(c, 0);
  const gm::Rom r2 = gmpc::SimProbe::makeRom(0x28, 0x0A11C233);
  EXPECT_EQ(gm::readSensor(probe, r2, &c), gm::ReadStatus::Ok);
  EXPECT_EQ(c, -350);
}

TEST(Ds18b20Bus, DistinguishesErrors) {
  gmpc::SimClock clock(1.0, true);
  gmpc::SimProbe probe(clock, 2);
  gm::startConversionAll(probe);
  bool timeout = false;
  gm::waitConversion(probe, 800, &timeout);
  const gm::Rom r1 = gmpc::SimProbe::makeRom(0x28, 0x1E8564FF);
  int16_t c = 0;
  probe.injectCrc(1, 1);
  EXPECT_EQ(gm::readSensor(probe, r1, &c), gm::ReadStatus::CrcError);
  EXPECT_EQ(gm::readSensor(probe, r1, &c), gm::ReadStatus::Ok);  // transient
  probe.setPresent(1, false);
  EXPECT_EQ(gm::readSensor(probe, r1, &c), gm::ReadStatus::Missing);
  const gm::Rom s20 = gmpc::SimProbe::makeRom(0x10, 1);
  EXPECT_EQ(gm::readSensor(probe, s20, &c), gm::ReadStatus::Unsupported);
  probe.setShorted(true);
  EXPECT_EQ(
      gm::readSensor(probe, gmpc::SimProbe::makeRom(0x28, 0x0A11C233), &c),
      gm::ReadStatus::BusError);
}

TEST(Ds18b20Bus, ConversionTimeout) {
  gmpc::SimClock clock(1.0, true);
  gmpc::SimProbe probe(clock, 1);
  gm::startConversionAll(probe);
  bool timeout = false;
  gm::waitConversion(probe, 300, &timeout);  // shorter than 750 ms
  EXPECT_TRUE(timeout);
}
