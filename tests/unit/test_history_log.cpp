// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "data/history_log.h"

namespace {
// Small NOR-flash emulation: 4 sectors x 8 records.
class MemFlash : public gm::HistoryStorage {
 public:
  explicit MemFlash(size_t sectors = 4, size_t sector = 256)
      : sector_(sector), data_(sectors * sector, 0xFF) {}
  size_t size() override { return data_.size(); }
  size_t sectorSize() override { return sector_; }
  bool read(size_t off, void* buf, size_t len) override {
    if (off + len > data_.size()) return false;
    std::memcpy(buf, &data_[off], len);
    return true;
  }
  bool write(size_t off, const void* buf, size_t len) override {
    if (off + len > data_.size()) return false;
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    for (size_t i = 0; i < len; ++i) data_[off + i] &= p[i];
    ++writes;
    return true;
  }
  bool eraseSector(size_t off) override {
    std::memset(&data_[off], 0xFF, sector_);
    ++erases;
    return true;
  }
  size_t sector_;
  std::vector<uint8_t> data_;
  int writes = 0;
  int erases = 0;
};

void row(int16_t base, int16_t out[8], uint8_t st[8]) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<int16_t>(base + i);
    st[i] = 0;
  }
}

std::vector<gm::HistoryRecord> all(gm::HistoryLog& log) {
  std::vector<gm::HistoryRecord> v;
  log.forEach(
      0, 0xFFFFFFFFu,
      [](void* ctx, const gm::HistoryRecord& r) {
        static_cast<std::vector<gm::HistoryRecord>*>(ctx)->push_back(r);
      },
      &v);
  return v;
}
}  // namespace

TEST(HistoryLog, EncodeDecodeRoundTrip) {
  gm::HistoryRecord r = {};
  r.seq = 77;
  r.epoch = 1795000000u;
  for (int i = 0; i < 8; ++i) {
    r.centi[i] = static_cast<int16_t>(-500 + i * 300);
    r.status[i] = static_cast<uint8_t>(i % 8);
  }
  r.centi[3] = gm::kHistoryMissing;
  uint8_t buf[gm::kHistoryRecordSize];
  gm::HistoryLog::encode(r, buf);
  gm::HistoryRecord d;
  ASSERT_TRUE(gm::HistoryLog::decode(buf, &d));
  EXPECT_EQ(d.seq, 77u);
  EXPECT_EQ(d.centi[0], -500);
  EXPECT_EQ(d.centi[3], gm::kHistoryMissing);
  EXPECT_EQ(d.status[7], 7);
  buf[20] ^= 0x01;  // bit flip -> CRC mismatch
  EXPECT_FALSE(gm::HistoryLog::decode(buf, &d));
}

TEST(HistoryLog, AppendWrapAndRestart) {
  MemFlash flash;
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  EXPECT_EQ(log.capacity(), 24u);  // one sector is always being recycled
  int16_t c[8];
  uint8_t st[8];
  for (int k = 0; k < 40; ++k) {  // 40 > 32 slots: wraps, oldest erased
    row(static_cast<int16_t>(k * 10), c, st);
    ASSERT_TRUE(log.append(1000u + static_cast<uint32_t>(k) * 900u, c, st));
  }
  auto v = all(log);
  ASSERT_GE(v.size(), 24u);
  for (size_t i = 1; i < v.size(); ++i) EXPECT_EQ(v[i].seq, v[i - 1].seq + 1);
  EXPECT_EQ(v.back().seq, 40u);
  EXPECT_EQ(v.back().centi[0], 390);

  // "Reboot": a fresh log on the same flash continues the sequence.
  gm::HistoryLog again;
  ASSERT_TRUE(again.begin(&flash));
  row(999, c, st);
  ASSERT_TRUE(again.append(99999u, c, st));
  auto w = all(again);
  EXPECT_EQ(w.back().seq, 41u);
  EXPECT_EQ(w.back().epoch, 99999u);
  for (size_t i = 1; i < w.size(); ++i) EXPECT_EQ(w[i].seq, w[i - 1].seq + 1);
}

TEST(HistoryLog, TornRecordIsSkippedAfterRestart) {
  MemFlash flash;
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  int16_t c[8];
  uint8_t st[8];
  for (int k = 0; k < 3; ++k) {
    row(static_cast<int16_t>(k), c, st);
    log.append(5000u + static_cast<uint32_t>(k), c, st);
  }
  // power loss during the 4th write: half of the slot programmed
  const uint8_t half[16] = {'G', 'H', 1, 0x12, 4, 0, 0, 0,
                            0,   0,   0, 0,    0, 0, 0, 0};
  flash.write(3 * gm::kHistoryRecordSize, half, sizeof(half));
  gm::HistoryLog after;
  ASSERT_TRUE(after.begin(&flash));
  row(50, c, st);
  ASSERT_TRUE(after.append(6000u, c, st));
  auto v = all(after);
  ASSERT_EQ(v.size(), 4u);  // 3 old + 1 new, torn slot ignored
  EXPECT_EQ(v.back().epoch, 6000u);
  EXPECT_EQ(v.back().seq, 4u);
}

TEST(HistoryLog, GarbageFlashStartsEmpty) {
  MemFlash flash;
  std::memset(flash.data_.data(), 0x5A, flash.data_.size());
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  int16_t c[8];
  uint8_t st[8];
  row(1, c, st);
  ASSERT_TRUE(log.append(1u, c, st));
  EXPECT_EQ(all(log).size(), 1u);
}

TEST(HistoryLog, ExportRawAndBucketed) {
  MemFlash flash(8, 4096);
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  int16_t c[8];
  uint8_t st[8];
  for (int k = 0; k < 100; ++k) {
    row(static_cast<int16_t>(1000 + k), c, st);
    if (k == 50) st[2] = 2;  // CRC error -> missing in export
    log.append(10000u + static_cast<uint32_t>(k) * 900u, c, st);
  }
  std::vector<uint8_t> out(16384);
  size_t len =
      gm::historyExport(log, 0, 0xFFFFFFFFu, 720, out.data(), out.size());
  ASSERT_EQ(len, 8u + 100u * 20u);
  EXPECT_EQ(std::memcmp(out.data(), "GMH1", 4), 0);
  EXPECT_EQ(out[4] | (out[5] << 8), 100);
  // row 50, point 3 missing
  const uint8_t* r50 = out.data() + 8 + 50 * 20;
  EXPECT_EQ(static_cast<int16_t>(r50[4 + 4] | (r50[5 + 4] << 8)),
            gm::kHistoryMissing);

  len = gm::historyExport(log, 10000u, 10000u + 99u * 900u, 10, out.data(),
                          out.size());
  const int rows = out[4] | (out[5] << 8);
  EXPECT_LE(rows, 10);
  EXPECT_GE(rows, 9);
  EXPECT_EQ(len, 8u + static_cast<size_t>(rows) * 20u);

  // range filter
  len = gm::historyExport(log, 10000u + 90u * 900u, 0xFFFFFFFFu, 720,
                          out.data(), out.size());
  EXPECT_EQ(out[4] | (out[5] << 8), 10);
}
