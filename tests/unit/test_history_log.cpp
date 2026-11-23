// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "data/history_log.h"

namespace {
// NOR-flash emulation (default: 4 sectors x 256 B).
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
    if (failWrites > 0) {
      --failWrites;
      return false;  // nothing programmed
    }
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    for (size_t i = 0; i < len; ++i) data_[off + i] &= p[i];
    ++writes;
    return true;
  }
  bool eraseSector(size_t off) override {
    if (failErases > 0) {
      --failErases;
      return false;  // old data stays
    }
    std::memset(&data_[off], 0xFF, sector_);
    ++erases;
    return true;
  }
  size_t sector_;
  std::vector<uint8_t> data_;
  int writes = 0;
  int erases = 0;
  int failWrites = 0;
  int failErases = 0;
};

gm::HistoryRecord rec(uint32_t epoch, int16_t base) {
  gm::HistoryRecord r = {};
  r.epoch = epoch;
  for (int i = 0; i < 8; ++i) r.centi[i] = static_cast<int16_t>(base + i);
  r.batteryMv = 3700;
  r.rssi = -67;
  r.flags = 1 | (1 << gm::kHistUploadShift);  // economy, last upload ok
  return r;
}

bool collect(void* ctx, const gm::HistoryRecord& r) {
  static_cast<std::vector<gm::HistoryRecord>*>(ctx)->push_back(r);
  return true;
}

std::vector<gm::HistoryRecord> all(gm::HistoryLog& log) {
  std::vector<gm::HistoryRecord> v;
  EXPECT_TRUE(log.forEach(0, 0xFFFFFFFFu, collect, &v));
  return v;
}

size_t records(gm::HistoryLog& log) {
  gm::HistoryStats s;
  EXPECT_TRUE(log.stats(&s));
  return s.records;
}

struct Row {
  uint32_t epoch;
  int16_t c0;
  uint16_t mv;
  int8_t rssi;
  uint8_t flags;
};

uint32_t le32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<Row> rows(const std::vector<uint8_t>& out, size_t len,
                      uint32_t* next = nullptr) {
  std::vector<Row> v;
  EXPECT_EQ(std::memcmp(out.data(), "GMH3", 4), 0);
  const size_t n = out[4] | (out[5] << 8);
  EXPECT_EQ(len, gm::kHistoryHeaderSize + n * gm::kHistoryRowSize);
  if (next != nullptr) *next = le32(out.data() + 8);
  for (size_t k = 0; k < n; ++k) {
    const uint8_t* p = out.data() + gm::kHistoryHeaderSize + k * 24;
    Row r;
    r.epoch = le32(p);
    r.c0 = static_cast<int16_t>(p[4] | (p[5] << 8));
    r.mv = static_cast<uint16_t>(p[20] | (p[21] << 8));
    r.rssi = static_cast<int8_t>(p[22]);
    r.flags = p[23];
    v.push_back(r);
  }
  return v;
}
}  // namespace

TEST(HistoryLog, KeyAndDeltaRoundTrip) {
  gm::HistoryRecord a = rec(1795000000u, 1500);
  a.centi[3] = gm::kHistoryMissing;
  a.status[3] = 2;
  uint8_t buf[gm::HistoryLog::kHistoryMaxRecord];
  ASSERT_EQ(gm::HistoryLog::encodeKey(a, 7, buf),
            static_cast<size_t>(gm::HistoryLog::kHistoryKeySize));
  gm::HistoryRecord s = {};
  uint32_t seq = 0;
  bool valid = false;
  ASSERT_EQ(gm::HistoryLog::decode(buf, sizeof(buf), &s, &seq, &valid), 34u);
  ASSERT_TRUE(valid);
  EXPECT_EQ(seq, 7u);
  EXPECT_EQ(s.epoch, a.epoch);
  EXPECT_EQ(s.centi[3], gm::kHistoryMissing);
  EXPECT_EQ(s.status[3], 2);
  EXPECT_EQ(s.batteryMv, 3700);

  gm::HistoryRecord b = a;  // 15 min later: two points and the battery moved
  b.epoch += 900;
  b.centi[0] = static_cast<int16_t>(b.centi[0] + 12);
  b.centi[3] = 1490;
  b.status[3] = 0;
  b.batteryMv = 3684;
  const size_t len = gm::HistoryLog::encodeDelta(s, b, buf);
  EXPECT_LE(len, 16u);  // vs 34 for a full record
  ASSERT_EQ(gm::HistoryLog::decode(buf, len, &s, &seq, &valid), len);
  ASSERT_TRUE(valid);
  EXPECT_EQ(s.epoch, b.epoch);
  EXPECT_EQ(s.centi[0], b.centi[0]);
  EXPECT_EQ(s.centi[3], 1490);
  EXPECT_EQ(s.status[3], 0);
  EXPECT_EQ(s.batteryMv, 3680);  // 10 mV steps
  EXPECT_EQ(s.rssi, -67);

  buf[3] ^= 0x01;  // bit flip -> CRC mismatch, length still known
  gm::HistoryRecord t = s;
  EXPECT_EQ(gm::HistoryLog::decode(buf, len, &t, &seq, &valid), len);
  EXPECT_FALSE(valid);
  const uint8_t blank[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(gm::HistoryLog::decode(blank, 4, &t, &seq, &valid), 0u);
}

TEST(HistoryLog, UnchangedMeasurementsAreNotStored) {
  MemFlash flash(8, 4096);
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  bool stored = false;
  for (int k = 0; k < 10; ++k) {
    gm::HistoryRecord r = rec(10000u + k * 900u, 1500);
    r.centi[1] = static_cast<int16_t>(r.centi[1] + (k % 2) * 6);  // 1 LSB
    r.batteryMv = static_cast<uint16_t>(3700 - (k % 3) * 10);
    ASSERT_TRUE(log.append(r, k ? r.epoch - 900 : 0, &stored));
    EXPECT_EQ(stored, k == 0);
  }
  EXPECT_EQ(records(log), 1u);
  // a real change: the plateau end (held state) is stored first
  ASSERT_TRUE(
      log.append(rec(10000u + 10 * 900u, 1520), 10000u + 9 * 900u, &stored));
  EXPECT_TRUE(stored);
  auto v = all(log);
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[1].epoch, 10000u + 9 * 900u);
  EXPECT_EQ(v[1].centi[0], 1500);  // held value
  EXPECT_EQ(v[2].centi[0], 1520);
}

TEST(HistoryLog, HeartbeatAndDeviceStateChanges) {
  MemFlash flash(8, 4096);
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  for (int k = 0; k <= 12; ++k) {  // 3 h unchanged -> heartbeat, no hold
    ASSERT_TRUE(log.append(rec(k * 900u, 1500), k ? (k - 1) * 900u : 0));
  }
  auto v = all(log);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v[1].epoch, 12u * 900u);

  gm::HistoryRecord r = rec(13 * 900u, 1500);
  r.flags = 2 | (2 << gm::kHistUploadShift);  // critical, upload failed
  bool stored = false;
  ASSERT_TRUE(log.append(r, 12 * 900u, &stored));
  EXPECT_TRUE(stored);
  r = rec(14 * 900u, 1500);
  r.flags = 2 | (2 << gm::kHistUploadShift);
  r.rssi = -80;  // signal dropped by 13 dB
  ASSERT_TRUE(log.append(r, 13 * 900u, &stored));
  EXPECT_TRUE(stored);
  r.epoch += 900;
  r.rssi = -77;  // within 6 dB
  ASSERT_TRUE(log.append(r, r.epoch - 900, &stored));
  EXPECT_FALSE(stored);
}

TEST(HistoryLog, RestartIsAlwaysStored) {
  MemFlash flash(8, 4096);
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  ASSERT_TRUE(log.append(rec(1000u, 1500), 0));
  gm::HistoryRecord r = rec(1900u, 1500);
  r.flags |= gm::kHistRestart;
  bool stored = false;
  ASSERT_TRUE(log.append(r, 0, &stored));
  EXPECT_TRUE(stored);
  // the restart mark alone is not a change
  ASSERT_TRUE(log.append(rec(2800u, 1500), 1900u, &stored));
  EXPECT_FALSE(stored);
  auto v = all(log);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_TRUE(v[1].flags & gm::kHistRestart);
}

TEST(HistoryLog, WrapsAndContinuesAfterRestart) {
  MemFlash flash;  // 4 x 256 B: a key and ~20 deltas per sector
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  for (int k = 0; k < 200; ++k) {  // every measurement changes
    ASSERT_TRUE(log.append(rec(1000u + k * 900u, static_cast<int16_t>(k * 20)),
                           k ? 1000u + (k - 1) * 900u : 0));
  }
  EXPECT_GT(flash.erases, 4);  // wrapped
  auto v = all(log);
  ASSERT_GT(v.size(), 40u);
  for (size_t i = 1; i < v.size(); ++i) {
    EXPECT_EQ(v[i].epoch, v[i - 1].epoch + 900u);
    EXPECT_EQ(v[i].centi[0], v[i - 1].centi[0] + 20);
  }
  EXPECT_EQ(v.back().centi[0], 199 * 20);

  // "Reboot": a fresh log replays the newest sector and continues the deltas.
  gm::HistoryLog again;
  ASSERT_TRUE(again.begin(&flash));
  gm::HistoryRecord last;
  ASSERT_TRUE(again.last(&last));
  EXPECT_EQ(last.epoch, 1000u + 199u * 900u);
  ASSERT_TRUE(again.append(rec(1000u + 200u * 900u, 4321), 0));
  auto w = all(again);
  EXPECT_EQ(w.back().centi[0], 4321);
  EXPECT_EQ(w.back().centi[7], 4328);
  EXPECT_EQ(w[w.size() - 2].centi[0], 199 * 20);
}

TEST(HistoryLog, TornRecordIsSkippedAfterRestart) {
  MemFlash flash;
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  for (int k = 0; k < 3; ++k) {
    log.append(rec(5000u + k * 900u, static_cast<int16_t>(1000 + k * 50)), 0);
  }
  gm::HistoryStats st;
  ASSERT_TRUE(log.stats(&st));
  const size_t end = st.usedBytes;  // all in sector 0
  // power loss while writing the 4th record: only its start is on flash
  const uint8_t half[4] = {0xD3, 12, 0x88, 0x0E};
  flash.write(end, half, sizeof(half));
  gm::HistoryLog after;
  ASSERT_TRUE(after.begin(&flash));
  ASSERT_TRUE(after.append(rec(9000u, 2000), 0));
  auto v = all(after);
  ASSERT_EQ(v.size(), 4u);  // 3 old + 1 new, torn record ignored
  EXPECT_EQ(v[2].centi[0], 1100);
  EXPECT_EQ(v.back().epoch, 9000u);
  EXPECT_EQ(v.back().centi[0], 2000);
}

TEST(HistoryLog, FailedWriteStartsNewSector) {
  MemFlash flash;
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  for (int k = 0; k < 3; ++k) {
    ASSERT_TRUE(
        log.append(rec(1000u + k * 900u, static_cast<int16_t>(k * 50)), 0));
  }
  flash.failWrites = 1;
  EXPECT_FALSE(log.append(rec(4000u, 500), 0));
  ASSERT_TRUE(log.append(rec(5000u, 600), 0));
  ASSERT_TRUE(log.append(rec(6000u, 700), 0));
  gm::HistoryLog after;  // what a reader (or the next wake) sees
  ASSERT_TRUE(after.begin(&flash));
  auto v = all(after);
  ASSERT_EQ(v.size(), 5u);
  EXPECT_EQ(v[3].epoch, 5000u);
  EXPECT_EQ(v[3].centi[0], 600);
  EXPECT_EQ(v[4].centi[0], 700);
}

TEST(HistoryLog, FailedEraseLeavesNoOldRowsInBetween) {
  MemFlash flash;
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  uint32_t t = 1000u;
  int k = 0;
  for (; flash.erases < 6; ++k, t += 900u) {  // wrapped once
    ASSERT_TRUE(log.append(rec(t, static_cast<int16_t>(k * 20)), 0));
  }
  flash.failErases = 1;  // the next sector keeps its old records
  while (flash.failErases > 0) {
    log.append(rec(t, static_cast<int16_t>(k * 20)), 0);
    ++k;
    t += 900u;
  }
  for (int j = 0; j < 3; ++j, ++k, t += 900u) {  // stale sector still there
    ASSERT_TRUE(log.append(rec(t, static_cast<int16_t>(k * 20)), 0));
  }
  auto v = all(log);
  ASSERT_GT(v.size(), 10u);
  for (size_t i = 1; i < v.size(); ++i) EXPECT_GT(v[i].epoch, v[i - 1].epoch);
  EXPECT_EQ(v.back().epoch, t - 900u);
}

TEST(HistoryLog, FailedHoldStillStoresTheChange) {
  MemFlash flash(8, 4096);
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  ASSERT_TRUE(log.append(rec(1000u, 1500), 0));
  flash.failWrites = 1;  // the plateau-end record
  bool stored = false;
  EXPECT_FALSE(
      log.append(rec(1000u + 4 * 900u, 1600), 1000u + 3 * 900u, &stored));
  EXPECT_TRUE(stored);
  gm::HistoryLog after;
  ASSERT_TRUE(after.begin(&flash));
  auto v = all(after);
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v.back().centi[0], 1600);
}

TEST(HistoryLog, GarbageAndOldFormatsStartEmpty) {
  MemFlash flash;
  std::memset(flash.data_.data(), 0x5A, flash.data_.size());
  // 0.2.x records (32 B, marker 0xA2) look like garbage to this format
  for (size_t off = 0; off < flash.data_.size(); off += 32) {
    flash.data_[off] = 0xA2;
  }
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  EXPECT_EQ(records(log), 0u);
  ASSERT_TRUE(log.append(rec(1u, 100), 0));
  ASSERT_TRUE(log.append(rec(901u, 300), 1u));
  EXPECT_EQ(all(log).size(), 2u);
}

TEST(HistoryLog, ExportPagesCarryInAndHold) {
  MemFlash flash(8, 4096);
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  for (int k = 0; k < 100; ++k) {
    log.append(rec(10000u + k * 900u, static_cast<int16_t>(1000 + k * 20)),
               k ? 10000u + (k - 1) * 900u : 0);
  }
  std::vector<uint8_t> out(4096);
  gm::HistoryExportRequest q = {};
  q.from = 0;
  q.to = 0xFFFFFFFFu;
  q.maxRows = 256;
  q.maxGapS = 3 * 3600 + 1800;
  size_t len = gm::historyExport(log, q, out.data(), out.size());
  uint32_t next = 1;
  auto r = rows(out, len, &next);
  ASSERT_EQ(r.size(), 100u);
  EXPECT_EQ(next, 0u);
  EXPECT_EQ(r[5].mv, 3700);
  EXPECT_EQ(r[5].rssi, -67);
  EXPECT_EQ(le32(out.data() + 12), 3u * 3600u + 1800u);  // maxGap

  // pages of 30 rows; the next page starts with the previous page's last row
  // (state at its start) -> the client deduplicates by time
  q.maxRows = 30;
  std::vector<uint32_t> seen;
  q.from = 10000u;
  for (int page = 0; page < 10; ++page) {
    len = gm::historyExport(log, q, out.data(), out.size());
    for (const Row& x : rows(out, len, &next)) {
      if (seen.empty() || x.epoch > seen.back()) seen.push_back(x.epoch);
    }
    if (next == 0) break;
    q.from = next;
  }
  EXPECT_EQ(next, 0u);
  EXPECT_EQ(seen.size(), 100u);

  // one "day" in the middle: carry-in row before `from`
  q.maxRows = 256;
  q.from = 10000u + 50 * 900u + 100;
  q.to = 10000u + 60 * 900u;
  len = gm::historyExport(log, q, out.data(), out.size());
  r = rows(out, len);
  ASSERT_EQ(r.size(), 11u);
  EXPECT_EQ(r[0].epoch, 10000u + 50 * 900u);

  // held state until the last measurement (not stored: unchanged)
  q.to = 0xFFFFFFFFu;
  q.holdUntil = 10000u + 99 * 900u + 1800;
  len = gm::historyExport(log, q, out.data(), out.size());
  r = rows(out, len);
  EXPECT_EQ(r.back().epoch, q.holdUntil);
  EXPECT_TRUE(r.back().flags & gm::kHistHold);
  EXPECT_EQ(r.back().c0, 1000 + 99 * 20);

  // empty range
  q.from = 1;
  q.to = 2;
  q.holdUntil = 0;
  len = gm::historyExport(log, q, out.data(), out.size());
  EXPECT_EQ(rows(out, len).size(), 0u);
}

// A month of a slowly warming silo with sensor noise: how much flash does
// the change-only log use?
TEST(HistoryLog, RealisticSiloMonthIsCompact) {
  MemFlash flash(48, 4096);  // the ESP32 partition: 192 KiB
  gm::HistoryLog log;
  ASSERT_TRUE(log.begin(&flash));
  uint32_t seed = 12345;
  auto noise = [&seed]() {  // -1, 0, +1 LSB
    seed = seed * 1103515245u + 12345u;
    return static_cast<int>((seed >> 16) % 3) - 1;
  };
  const int days = 30;
  const int perDay = 96;
  int stored = 0;
  for (int k = 0; k < days * perDay; ++k) {
    const double d = k / 96.0;
    gm::HistoryRecord r = {};
    r.epoch = 1795000000u + k * 900u;
    for (int i = 0; i < 8; ++i) {
      // 0.3 degC/day drift + a daily wave near the top of the grain
      const double c = 12.0 + 0.3 * d +
                       (i < 2 ? 0.4 : 0.05) * std::sin(2 * 3.14159265 * d + i);
      const long lsb = std::lround(c / 0.0625) + noise();
      r.centi[i] = static_cast<int16_t>(std::lround(lsb * 6.25));
    }
    r.batteryMv = static_cast<uint16_t>(4100 - d * 8 + noise() * 10);
    r.rssi = static_cast<int8_t>(-70 + noise() * 2);
    r.flags = 1 << gm::kHistUploadShift;
    bool s = false;
    ASSERT_TRUE(log.append(r, k ? r.epoch - 900 : 0, &s));
    stored += s ? 1 : 0;
  }
  gm::HistoryStats st;
  ASSERT_TRUE(log.stats(&st));
  const double perDayBytes = static_cast<double>(st.usedBytes) / days;
  const double daysFit = st.capacityBytes / perDayBytes;
  std::printf(
      "      %d of %d measurements stored as %zu records, %zu B used, "
      "%.0f B/day -> %.0f days in %zu B (v2: 3072 B/day, 62 days)\n",
      stored, days * perDay, st.records, st.usedBytes, perDayBytes, daysFit,
      st.capacityBytes);
  EXPECT_LT(perDayBytes, 600.0);  // v2: 3072 B/day
  EXPECT_GT(daysFit, 300.0);
}
