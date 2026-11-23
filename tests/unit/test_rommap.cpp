// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <filesystem>

#include "app/config.h"
#include "data/rommap.h"
#include "platform/file_store.h"
#include "sim/sim_probe.h"

namespace {
gm::Rom rom(uint32_t serial, uint8_t family = 0x28) {
  return gmpc::SimProbe::makeRom(family, serial);
}
}  // namespace

TEST(RomMap, AssignReplacesSamePointAndSameRom) {
  gm::RomMap m;
  const char* err = nullptr;
  ASSERT_TRUE(m.assign(rom(1), 3, 120, &err));
  ASSERT_TRUE(m.assign(rom(2), 3, 80, &err));  // point 3 moves to rom 2
  EXPECT_EQ(m.size(), 1u);
  EXPECT_EQ(m.pointOf(rom(1)), 0);
  EXPECT_EQ(m.pointOf(rom(2)), 3);
  ASSERT_TRUE(m.assign(rom(2), 5, 80, &err));  // rom 2 moves to point 5
  EXPECT_EQ(m.size(), 1u);
  EXPECT_EQ(m.pointOf(rom(2)), 5);
}

TEST(RomMap, RejectsInvalid) {
  gm::RomMap m;
  const char* err = nullptr;
  EXPECT_FALSE(m.assign(rom(1), 0, 0, &err));
  EXPECT_FALSE(m.assign(rom(1), 9, 0, &err));
  EXPECT_FALSE(m.assign(rom(1, 0x10), 1, 0, &err));  // DS18S20 not measured
  gm::Rom bad = rom(1);
  bad.b[7] ^= 0xFF;
  EXPECT_FALSE(m.assign(bad, 1, 0, &err));
  EXPECT_EQ(m.size(), 0u);
}

TEST(RomMap, SerializeParseRoundTrip) {
  gm::RomMap m;
  m.assign(rom(10), 1, 0, nullptr);
  m.assign(rom(11), 8, 1450, nullptr);
  m.assign(rom(12), 4, -1, nullptr);
  char text[256];
  ASSERT_TRUE(m.serialize(text, sizeof(text)));
  gm::RomMap n;
  ASSERT_TRUE(n.parse(text));
  ASSERT_EQ(n.size(), 3u);
  EXPECT_EQ(n.pointOf(rom(11)), 8);
  EXPECT_EQ(n.findPoint(8)->heightCm, 1450);
  EXPECT_EQ(n.findPoint(4)->heightCm, -1);
}

TEST(RomMap, ParseRejectsGarbageAndKeepsOldContent) {
  gm::RomMap m;
  m.assign(rom(10), 1, 0, nullptr);
  EXPECT_FALSE(m.parse("28FF:1:0"));
  EXPECT_FALSE(m.parse("ZZFF641E8516030D:1:0"));
  char hex[17];
  std::string s =
      std::string(gm::romToHex(rom(10), hex, sizeof(hex))) + ":12:0";
  EXPECT_FALSE(m.parse(s.c_str()));  // point out of range
  EXPECT_EQ(m.size(), 1u);
  EXPECT_TRUE(m.parse(""));  // empty map is valid
  EXPECT_EQ(m.size(), 0u);
}

TEST(RomMap, AutoAssignFillsFreePointsInRomOrder) {
  gm::RomMap m;
  m.assign(rom(50), 1, 0, nullptr);
  const gm::Rom found[] = {rom(50), rom(7, 0x10), rom(60), rom(70)};
  EXPECT_EQ(m.autoAssign(found, 4), 2u);  // 0x10 skipped, rom50 already mapped
  EXPECT_EQ(m.pointOf(rom(60)), 2);
  EXPECT_EQ(m.pointOf(rom(70)), 3);
}

// Mapping must survive a restart: save through the store, read back with a
// fresh store instance (= next boot).
TEST(RomMap, PersistsAcrossRestart) {
  const auto path =
      std::filesystem::temp_directory_path() / "gm_rommap_persist.conf";
  std::filesystem::remove(path);
  {
    gmpc::FileStore store(path.string());
    ASSERT_TRUE(store.load());
    gm::RomMap m;
    m.assign(rom(21), 2, 300, nullptr);
    m.assign(rom(22), 7, 900, nullptr);
    ASSERT_TRUE(gm::romMapSave(store, m));
  }
  {
    gmpc::FileStore store(path.string());
    ASSERT_TRUE(store.load());
    gm::Config cfg;
    gm::configDefaults(&cfg, "mock://x");
    gm::RomMap m;
    const char* err = nullptr;
    EXPECT_TRUE(gm::configLoad(store, &cfg, &m, &err));
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m.pointOf(rom(22)), 7);
    EXPECT_EQ(m.findPoint(2)->heightCm, 300);
  }
  std::filesystem::remove(path);
}
