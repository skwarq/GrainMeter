// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "onewire/onewire.h"
#include "onewire/rom.h"
#include "sim/sim_probe.h"
#include "util/crc.h"

TEST(Crc8, MaximApplicationNoteVector) {
  const uint8_t rom[] = {0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00};
  EXPECT_EQ(gm::crc8Maxim(rom, sizeof(rom)), 0xA2);
}

TEST(Crc8, PowerOnScratchpadVector) {
  const uint8_t sp[] = {0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x1C};
  EXPECT_EQ(gm::crc8Maxim(sp, 8), 0x1C);
  EXPECT_EQ(gm::crc8Maxim(sp, 9), 0x00);  // CRC over data+CRC is zero
}

TEST(Crc32, KnownVector) { EXPECT_EQ(gm::crc32("123456789", 9), 0xCBF43926u); }

TEST(Rom, HexRoundTripAndFamily) {
  gm::Rom rom;
  ASSERT_TRUE(gm::romFromHex("28FF641E8516030D", &rom));
  EXPECT_EQ(rom.family(), 0x28);
  char hex[17];
  EXPECT_STREQ(gm::romToHex(rom, hex, sizeof(hex)), "28FF641E8516030D");
  ASSERT_TRUE(gm::romFromHex("28:ff:64:1e:85:16:03:0d", &rom));
  EXPECT_STREQ(gm::romToHex(rom, hex, sizeof(hex)), "28FF641E8516030D");
  EXPECT_FALSE(gm::romFromHex("28FF641E8516030", &rom));    // 15 digits
  EXPECT_FALSE(gm::romFromHex("28FF641E8516030DA", &rom));  // 17 digits
  EXPECT_FALSE(gm::romFromHex("28FF641E851603XZ", &rom));
}

TEST(Rom, CrcValidation) {
  gm::Rom rom = gmpc::SimProbe::makeRom(0x28, 0x12345678);
  EXPECT_TRUE(rom.crcValid());
  rom.b[3] ^= 0x01;
  EXPECT_FALSE(rom.crcValid());
  gm::Rom zero = {};
  EXPECT_FALSE(zero.crcValid());  // all-zero ROM has CRC 0 but is not a device
}

TEST(Rom, FamilyNames) {
  EXPECT_STREQ(gm::familyName(0x28), "DS18B20");
  EXPECT_STREQ(gm::familyName(0x10), "DS18S20/DS1820");
  EXPECT_STREQ(gm::familyName(0x22), "DS1822");
  EXPECT_STREQ(gm::familyName(0x42), "unknown");
  EXPECT_TRUE(gm::familySupported(0x28));
  EXPECT_FALSE(gm::familySupported(0x10));
  EXPECT_FALSE(gm::familySupported(0x22));
}

class SearchTest : public ::testing::Test {
 protected:
  gmpc::SimClock clock{1.0, true};
};

TEST_F(SearchTest, FindsAllEightSorted) {
  gmpc::SimProbe probe(clock, 8);
  gm::Rom roms[16];
  const gm::SearchResult r = gm::searchRom(probe, roms, 16);
  EXPECT_TRUE(r.presence);
  EXPECT_FALSE(r.busError);
  EXPECT_EQ(r.found, 8u);
  EXPECT_EQ(r.crcErrors, 0u);
  for (size_t i = 1; i < r.found; ++i) {
    EXPECT_TRUE(roms[i - 1] < roms[i]);
  }
}

TEST_F(SearchTest, NoDevices) {
  gmpc::SimProbe probe(clock, 0);
  gm::Rom roms[16];
  const gm::SearchResult r = gm::searchRom(probe, roms, 16);
  EXPECT_FALSE(r.presence);
  EXPECT_EQ(r.found, 0u);
}

TEST_F(SearchTest, MissingSensorIsNotListed) {
  gmpc::SimProbe probe(clock, 8);
  probe.setPresent(4, false);
  gm::Rom roms[16];
  EXPECT_EQ(gm::searchRom(probe, roms, 16).found, 7u);
}

TEST_F(SearchTest, ListsOtherFamilies) {
  gmpc::SimProbe probe(clock, 8);
  probe.setUnsupportedDevice(true);
  gm::Rom roms[16];
  const gm::SearchResult r = gm::searchRom(probe, roms, 16);
  ASSERT_EQ(r.found, 9u);
  EXPECT_EQ(roms[0].family(), 0x10);  // sorted: 0x10 < 0x28
}

TEST_F(SearchTest, TruncatesAtCapacity) {
  gmpc::SimProbe probe(clock, 8);
  gm::Rom roms[3];
  const gm::SearchResult r = gm::searchRom(probe, roms, 3);
  EXPECT_EQ(r.found, 3u);
  EXPECT_TRUE(r.truncated);
}

TEST_F(SearchTest, ShortedLineIsBusError) {
  gmpc::SimProbe probe(clock, 8);
  probe.setShorted(true);
  gm::Rom roms[16];
  const gm::SearchResult r = gm::searchRom(probe, roms, 16);
  EXPECT_EQ(r.found, 0u);
  // reads return 0/0 for every bit: path of all zeros, CRC of zero ROM is
  // rejected as invalid.
  EXPECT_GE(r.crcErrors + (r.busError ? 1u : 0u), 1u);
}
