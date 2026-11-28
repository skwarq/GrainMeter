// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "support/test_device.h"

namespace {
gm::Rom deviceRom(int index) {
  static const uint32_t serials[] = {0x1E8564FF, 0x0A11C233, 0x7F0001A4,
                                     0x33B2C1D0, 0x5D9E0042, 0x00A1B2C3,
                                     0x6C6C1F1F, 0x2BADF00D};
  return gmpc::SimProbe::makeRom(0x28, serials[index - 1]);
}

class CountingStore : public gm::KeyValueStore {
 public:
  explicit CountingStore(gm::KeyValueStore& store) : store_(store) {}
  bool get(const char* key, char* out, size_t cap) override {
    return store_.get(key, out, cap);
  }
  bool set(const char* key, const char* value) override {
    ++writes;
    return !failSet && store_.set(key, value);
  }
  bool commit() override {
    ++commits;
    return !failCommit && store_.commit();
  }
  size_t writes = 0;
  size_t commits = 0;
  bool failSet = false;
  bool failCommit = false;

 private:
  gm::KeyValueStore& store_;
};

class CountingBus : public gm::OneWireBus {
 public:
  explicit CountingBus(gm::OneWireBus& bus) : bus_(bus) {}
  bool reset() override {
    ++resets;
    if (shortOnRead && resets > gm::kMaxPoints + 1) return true;
    return bus_.reset();
  }
  void writeBit(bool bit) override {
    ++writes;
    bus_.writeBit(bit);
  }
  bool readBit() override {
    ++reads;
    if (shortOnRead && resets > gm::kMaxPoints + 1) return false;
    if (missingOnRead && resets > gm::kMaxPoints + 1) return true;
    return bus_.readBit();
  }
  void sleepMs(uint32_t ms) override { bus_.sleepMs(ms); }
  size_t resets = 0;
  size_t writes = 0;
  size_t reads = 0;
  bool shortOnRead = false;
  bool missingOnRead = false;

 private:
  gm::OneWireBus& bus_;
};

class ThTlAutomap : public testing::Test {
 protected:
  ThTlAutomap()
      : t("thtl"), store(*t.dev->app().ports().store), bus(t.dev->probe()) {
    gm::Ports ports = t.dev->app().ports();
    ports.store = &store;
    ports.bus = &bus;
    ports.history = nullptr;
    app = std::make_unique<gm::GrainMeter>(
        ports, &state, gm::ConfigLimits{5, true}, "mock://thingspeak");
  }
  void SetUp() override {
    ASSERT_TRUE(t.started);
    ASSERT_TRUE(app->begin());
  }
  void number(int index, uint8_t point) {
    ASSERT_TRUE(t.dev->probe().setPointNumber(index, point, point));
  }
  void assign(int index, int point, int height) {
    char hex[17], err[128];
    ASSERT_TRUE(app->mapAssign(gm::romToHex(deviceRom(index), hex, sizeof(hex)),
                               point, height, err, sizeof(err)))
        << err;
  }
  std::string status() {
    char text[16384];
    gm::JsonWriter w(text, sizeof(text));
    app->writeStatus(w);
    return text;
  }
  TestDevice t;
  CountingStore store;
  CountingBus bus;
  gm::RetainedState state = {};
  std::unique_ptr<gm::GrainMeter> app;
};
}  // namespace

TEST_F(ThTlAutomap, MeasureAssignsNumberedSensors) {
  const gm::ScanReport scan = app->scan();
  ASSERT_EQ(scan.search.found, 8u);
  for (int i = 1; i <= 8; ++i) {
    for (size_t j = 0; j < scan.search.found; ++j) {
      if (deviceRom(i) == scan.roms[j]) {
        number(i, static_cast<uint8_t>(8 - j));
        ASSERT_TRUE(t.dev->probe().setTemperature(i, i));
      }
    }
  }
  const gm::MeasureReport& m = app->measure(true);
  ASSERT_EQ(app->romMap().size(), 8u);
  ASSERT_EQ(state.count, 1u);
  for (size_t j = 0; j < scan.search.found; ++j) {
    const uint8_t point = static_cast<uint8_t>(8 - j);
    EXPECT_EQ(app->romMap().pointOf(scan.roms[j]), point);
  }
  for (size_t i = 0; i < m.count; ++i) {
    const gm::SensorReading& r = m.readings[i];
    ASSERT_NE(r.point, 0);
    EXPECT_EQ(state.at(0).centi[r.point - 1], r.centi);
    EXPECT_EQ(state.at(0).status[r.point - 1],
              static_cast<uint8_t>(gm::ReadStatus::Ok));
  }
  EXPECT_EQ(store.commits, 1u);
  app->measure(true);
  EXPECT_EQ(store.commits, 1u);
  EXPECT_EQ(store.writes, 1u);
  t.dev->restart("TH/TL persistence");
  for (uint8_t p = 1; p <= 8; ++p) {
    ASSERT_NE(t.dev->app().romMap().findPoint(p), nullptr);
    EXPECT_TRUE(t.dev->app().romMap().findPoint(p)->rom ==
                app->romMap().findPoint(p)->rom);
  }
}

TEST_F(ThTlAutomap, ReplacementTakesPoint) {
  number(1, 2);
  ASSERT_TRUE(t.dev->probe().setPresent(2, false));
  app->measure(true);
  ASSERT_EQ(app->romMap().pointOf(deviceRom(1)), 2);
  assign(1, 2, 120);
  ASSERT_TRUE(t.dev->probe().setPresent(1, false));
  ASSERT_TRUE(t.dev->probe().setPresent(2, true));
  number(2, 2);
  ASSERT_TRUE(t.dev->probe().setTemperature(2, -3.5));
  const gm::MeasureReport& m = app->measure(true);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(1)), 0);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(2)), 2);
  ASSERT_NE(app->romMap().findPoint(2), nullptr);
  EXPECT_EQ(app->romMap().findPoint(2)->heightCm, -1);
  EXPECT_EQ(state.at(1).centi[1], -350);
  EXPECT_EQ(state.at(1).status[1], static_cast<uint8_t>(gm::ReadStatus::Ok));
  for (size_t i = 0; i < m.count; ++i) {
    if (m.readings[i].rom == deviceRom(1)) {
      EXPECT_EQ(m.readings[i].point, 0);
    }
  }
}

TEST_F(ThTlAutomap, MapAutoRescansBeforeTakeover) {
  number(1, 2);
  ASSERT_TRUE(t.dev->probe().setPresent(2, false));
  app->measure(true);
  ASSERT_EQ(app->romMap().pointOf(deviceRom(1)), 2);
  assign(1, 2, 120);
  // A stale scan sees the owner absent and a replacement numbered 2 ...
  ASSERT_TRUE(t.dev->probe().setPresent(1, false));
  ASSERT_TRUE(t.dev->probe().setPresent(2, true));
  number(2, 2);
  app->scan();
  // ... but the owner is back when automap runs: it keeps its point.
  ASSERT_TRUE(t.dev->probe().setPresent(1, true));
  app->mapAuto();
  EXPECT_EQ(app->romMap().pointOf(deviceRom(1)), 2);
  ASSERT_NE(app->romMap().findPoint(2), nullptr);
  EXPECT_EQ(app->romMap().findPoint(2)->heightCm, 120);
  EXPECT_NE(app->romMap().pointOf(deviceRom(2)), 2);
}

TEST_F(ThTlAutomap, MixedFallsBackToRomOrder) {
  for (int i = 1; i <= 4; ++i) number(i, static_cast<uint8_t>(9 - i));
  app->measure(false);
  ASSERT_EQ(app->romMap().size(), 4u);
  const gm::ScanReport scan = app->scan();
  EXPECT_EQ(app->mapAuto(), 4u);
  for (int i = 1; i <= 4; ++i) {
    EXPECT_EQ(app->romMap().pointOf(deviceRom(i)), 9 - i);
  }
  uint8_t point = 1;
  for (size_t j = 0; j < scan.search.found; ++j) {
    bool numbered = false;
    for (int i = 1; i <= 4; ++i) numbered |= scan.roms[j] == deviceRom(i);
    if (!numbered) {
      EXPECT_EQ(app->romMap().pointOf(scan.roms[j]), point++);
    }
  }
}

TEST_F(ThTlAutomap, ManualMappingWins) {
  assign(1, 3, 120);
  number(1, 5);
  number(2, 3);
  app->measure(false);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(1)), 3);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(2)), 0);
  EXPECT_EQ(app->romMap().findPoint(3)->heightCm, 120);
  EXPECT_EQ(store.commits, 1u);
}

TEST_F(ThTlAutomap, MapAutoUsesNumbersWithoutConversion) {
  for (int i = 1; i <= 8; ++i) number(i, static_cast<uint8_t>(9 - i));
  const uint32_t before = t.dev->clock().monoS();
  EXPECT_EQ(app->mapAuto(), 8u);
  EXPECT_EQ(t.dev->clock().monoS(), before);
  for (int i = 1; i <= 8; ++i) {
    EXPECT_EQ(app->romMap().pointOf(deviceRom(i)), 9 - i);
  }
  EXPECT_EQ(store.commits, 1u);
  EXPECT_EQ(app->mapAuto(), 0u);
  EXPECT_EQ(store.commits, 1u);
}

TEST_F(ThTlAutomap, DuplicateNumbersStayUnmappedEvenWithFallback) {
  number(1, 2);
  number(2, 2);
  app->measure(false);
  EXPECT_EQ(app->romMap().size(), 0u);
  const std::string error = "map: TH/TL point 2 claimed by several sensors";
  const std::string text = status();
  const size_t pos = text.find(error);
  ASSERT_NE(pos, std::string::npos);
  EXPECT_EQ(text.find(error, pos + error.size()), std::string::npos);
  EXPECT_EQ(app->mapAuto(), 6u);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(1)), 0);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(2)), 0);
}

TEST_F(ThTlAutomap, BadScratchpadDoesNotAssign) {
  number(1, 2);
  ASSERT_TRUE(t.dev->probe().injectCrc(1, 1));
  app->measure(false);
  EXPECT_EQ(app->romMap().size(), 0u);
  EXPECT_EQ(store.commits, 0u);
  bus.shortOnRead = true;
  bus.resets = 0;
  app->measure(false);
  EXPECT_EQ(app->romMap().size(), 0u);
  bus.shortOnRead = false;
  bus.missingOnRead = true;
  bus.resets = 0;
  app->measure(false);
  EXPECT_EQ(app->romMap().size(), 0u);
  bus.missingOnRead = false;
  app->measure(false);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(1)), 2);
}

TEST_F(ThTlAutomap, StorageFailureKeepsMapAndSampleUnchanged) {
  assign(1, 2, 120);
  ASSERT_TRUE(t.dev->probe().setPresent(1, false));
  number(2, 2);
  store.failSet = true;
  app->measure(true);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(1)), 2);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(2)), 0);
  EXPECT_EQ(app->romMap().findPoint(2)->heightCm, 120);
  EXPECT_EQ(state.at(0).status[1],
            static_cast<uint8_t>(gm::ReadStatus::Missing));
  EXPECT_NE(status().find("map: storage write failed"), std::string::npos);
  store.failSet = false;
  store.failCommit = true;
  EXPECT_EQ(app->mapAuto(), 0u);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(1)), 2);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(2)), 0);
  store.failCommit = false;
  app->measure(true);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(2)), 2);
}

TEST_F(ThTlAutomap, MeasureUsesNoExtraBusTraffic) {
  app->measure(false);
  const size_t resets = bus.resets;
  const size_t writes = bus.writes;
  const size_t reads = bus.reads;
  number(1, 2);
  bus.resets = bus.writes = bus.reads = 0;
  app->measure(false);
  EXPECT_EQ(app->romMap().pointOf(deviceRom(1)), 2);
  EXPECT_EQ(bus.resets, resets);
  EXPECT_EQ(bus.writes, writes);
  EXPECT_EQ(bus.reads, reads);
}
