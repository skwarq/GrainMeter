// SPDX-License-Identifier: MIT
#include "data/history_log.h"

#include <string.h>

#include "util/crc.h"
#include "util/log.h"

namespace gm {

static const char* kTag = "history";
static const uint8_t kMagic0 = 'G';
static const uint8_t kMagic1 = 'H';
static const uint8_t kVersion = 1;

static void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

static uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static bool blank(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i] != 0xFF) return false;
  }
  return true;
}

void HistoryLog::encode(const HistoryRecord& r,
                        uint8_t out[kHistoryRecordSize]) {
  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = kVersion;
  put32(out + 4, r.seq);
  put32(out + 8, r.epoch);
  for (int i = 0; i < kMaxPoints; ++i) {
    const uint16_t v = static_cast<uint16_t>(r.centi[i]);
    out[12 + 2 * i] = static_cast<uint8_t>(v);
    out[13 + 2 * i] = static_cast<uint8_t>(v >> 8);
  }
  for (int i = 0; i < kMaxPoints / 2; ++i) {
    out[28 + i] = static_cast<uint8_t>((r.status[2 * i] & 0x0F) |
                                       ((r.status[2 * i + 1] & 0x0F) << 4));
  }
  out[3] = crc8Maxim(out + 4, kHistoryRecordSize - 4);
}

bool HistoryLog::decode(const uint8_t in[kHistoryRecordSize],
                        HistoryRecord* r) {
  if (in[0] != kMagic0 || in[1] != kMagic1 || in[2] != kVersion ||
      in[3] != crc8Maxim(in + 4, kHistoryRecordSize - 4)) {
    return false;
  }
  r->seq = get32(in + 4);
  r->epoch = get32(in + 8);
  for (int i = 0; i < kMaxPoints; ++i) {
    r->centi[i] = static_cast<int16_t>(
        static_cast<uint16_t>(in[12 + 2 * i] | (in[13 + 2 * i] << 8)));
  }
  for (int i = 0; i < kMaxPoints / 2; ++i) {
    r->status[2 * i] = static_cast<uint8_t>(in[28 + i] & 0x0F);
    r->status[2 * i + 1] = static_cast<uint8_t>(in[28 + i] >> 4);
  }
  return true;
}

size_t HistoryLog::sectors() const {
  return st_ != nullptr ? st_->size() / st_->sectorSize() : 0;
}

size_t HistoryLog::perSector() const {
  return st_ != nullptr ? st_->sectorSize() / kHistoryRecordSize : 0;
}

size_t HistoryLog::capacity() const {
  return ok_ ? (sectors() - 1) * perSector() : 0;
}

bool HistoryLog::begin(HistoryStorage* storage) {
  st_ = storage;
  ok_ = false;
  if (st_ == nullptr || st_->sectorSize() < 2 * kHistoryRecordSize ||
      st_->sectorSize() % kHistoryRecordSize != 0 || sectors() < 2) {
    return false;
  }
  const size_t sec = st_->sectorSize();
  // Newest sector = the one whose first record has the highest sequence.
  uint8_t buf[kHistoryRecordSize];
  HistoryRecord r;
  size_t newest = 0;
  uint32_t newestSeq = 0;
  bool any = false;
  for (size_t s = 0; s < sectors(); ++s) {
    if (!st_->read(s * sec, buf, sizeof(buf))) {
      GM_LOGE(kTag, "read failed");
      return false;
    }
    if (decode(buf, &r) && (!any || r.seq > newestSeq)) {
      any = true;
      newest = s;
      newestSeq = r.seq;
    }
  }
  if (!any) {
    next_ = 0;  // empty log: the first append erases sector 0
    nextSeq_ = 1;
  } else {
    // Write position: first blank slot after the last used one in the
    // newest sector (torn / corrupt slots are skipped, never rewritten).
    size_t pos = newest * sec;
    const size_t end = pos + sec;
    uint32_t maxSeq = newestSeq;
    size_t nextPos = end;
    for (size_t off = pos; off < end; off += kHistoryRecordSize) {
      if (!st_->read(off, buf, sizeof(buf))) return false;
      if (blank(buf, sizeof(buf))) {
        if (nextPos == end) nextPos = off;
        continue;
      }
      nextPos = end;  // a used slot after a blank one: continue behind it
      if (decode(buf, &r) && r.seq > maxSeq) maxSeq = r.seq;
    }
    next_ = nextPos % st_->size();
    nextSeq_ = maxSeq + 1;
  }
  ok_ = true;
  return true;
}

bool HistoryLog::append(uint32_t epoch, const int16_t centi[kMaxPoints],
                        const uint8_t status[kMaxPoints]) {
  if (!ok_) return false;
  const size_t sec = st_->sectorSize();
  if (next_ % sec == 0 && !st_->eraseSector(next_)) {
    GM_LOGE(kTag, "erase failed at 0x%lx", static_cast<unsigned long>(next_));
    return false;
  }
  HistoryRecord r;
  r.seq = nextSeq_;
  r.epoch = epoch;
  memcpy(r.centi, centi, sizeof(r.centi));
  memcpy(r.status, status, sizeof(r.status));
  uint8_t buf[kHistoryRecordSize];
  encode(r, buf);
  bool ok = st_->write(next_, buf, sizeof(buf));
  uint8_t check[kHistoryRecordSize];
  ok = ok && st_->read(next_, check, sizeof(check)) &&
       memcmp(buf, check, sizeof(buf)) == 0;
  // A failed slot is skipped either way so that it is never rewritten.
  next_ = (next_ + kHistoryRecordSize) % st_->size();
  ++nextSeq_;
  if (!ok) {
    GM_LOGE(kTag, "write failed");
  }
  return ok;
}

bool HistoryLog::forEach(uint32_t from, uint32_t to, Visitor fn, void* ctx) {
  if (!ok_) return false;
  const size_t sec = st_->sectorSize();
  const size_t n = sectors();
  // Order sectors by the sequence of their first record (oldest first).
  uint32_t firstSeq[256];
  size_t order[256];
  size_t used = 0;
  uint8_t buf[kHistoryRecordSize];
  HistoryRecord r;
  for (size_t s = 0; s < n && s < 256; ++s) {
    if (!st_->read(s * sec, buf, sizeof(buf))) return false;
    if (!decode(buf, &r)) continue;
    size_t k = used++;
    while (k > 0 && firstSeq[k - 1] > r.seq) {
      firstSeq[k] = firstSeq[k - 1];
      order[k] = order[k - 1];
      --k;
    }
    firstSeq[k] = r.seq;
    order[k] = s;
  }
  count_ = 0;
  oldestEpoch_ = 0;
  uint8_t block[512];
  for (size_t i = 0; i < used; ++i) {
    for (size_t off = 0; off < sec; off += sizeof(block)) {
      const size_t len = sec - off < sizeof(block) ? sec - off : sizeof(block);
      if (!st_->read(order[i] * sec + off, block, len)) return false;
      for (size_t k = 0; k + kHistoryRecordSize <= len;
           k += kHistoryRecordSize) {
        if (!decode(block + k, &r)) continue;
        ++count_;
        if (oldestEpoch_ == 0) oldestEpoch_ = r.epoch;
        if (r.epoch >= from && r.epoch <= to && fn != nullptr) fn(ctx, r);
      }
    }
  }
  return true;
}

// ------------------------------------------------------------- export ----

namespace {
struct ExportCtx {
  uint32_t from;
  uint32_t span;  // seconds per bucket (0 = no bucketing)
  size_t maxRows;
  uint8_t* out;
  size_t cap;
  size_t rows;
  bool overflow;
  // current bucket
  bool open;
  uint32_t bucket;
  uint64_t sumEpoch;
  uint32_t n;
  int32_t sum[kMaxPoints];
  uint16_t cnt[kMaxPoints];
};

void putRow(ExportCtx* c, uint32_t epoch, const int16_t centi[kMaxPoints]) {
  const size_t off = kHistoryHeaderSize + c->rows * kHistoryRowSize;
  if (c->rows >= c->maxRows || off + kHistoryRowSize > c->cap) {
    c->overflow = true;
    return;
  }
  put32(c->out + off, epoch);
  for (int i = 0; i < kMaxPoints; ++i) {
    const uint16_t v = static_cast<uint16_t>(centi[i]);
    c->out[off + 4 + 2 * i] = static_cast<uint8_t>(v);
    c->out[off + 5 + 2 * i] = static_cast<uint8_t>(v >> 8);
  }
  ++c->rows;
}

void flushBucket(ExportCtx* c) {
  if (!c->open) return;
  int16_t avg[kMaxPoints];
  for (int i = 0; i < kMaxPoints; ++i) {
    avg[i] = c->cnt[i] == 0 ? static_cast<int16_t>(kHistoryMissing)
                            : static_cast<int16_t>(c->sum[i] / c->cnt[i]);
  }
  putRow(c, static_cast<uint32_t>(c->sumEpoch / c->n), avg);
  c->open = false;
}

void visit(void* ctx, const HistoryRecord& r) {
  ExportCtx* c = static_cast<ExportCtx*>(ctx);
  if (c->span == 0) {
    int16_t v[kMaxPoints];
    for (int i = 0; i < kMaxPoints; ++i) {
      v[i] =
          r.status[i] == 0 ? r.centi[i] : static_cast<int16_t>(kHistoryMissing);
    }
    putRow(c, r.epoch, v);
    return;
  }
  const uint32_t b = (r.epoch - c->from) / c->span;
  if (c->open && b != c->bucket) flushBucket(c);
  if (!c->open) {
    c->open = true;
    c->bucket = b;
    c->sumEpoch = 0;
    c->n = 0;
    memset(c->sum, 0, sizeof(c->sum));
    memset(c->cnt, 0, sizeof(c->cnt));
  }
  c->sumEpoch += r.epoch;
  ++c->n;
  for (int i = 0; i < kMaxPoints; ++i) {
    if (r.status[i] == 0 && r.centi[i] != kHistoryMissing) {
      c->sum[i] += r.centi[i];
      ++c->cnt[i];
    }
  }
}

void countVisit(void* ctx, const HistoryRecord&) {
  ++*static_cast<size_t*>(ctx);
}
}  // namespace

size_t historyExport(HistoryLog& log, uint32_t from, uint32_t to,
                     size_t maxRows, uint8_t* out, size_t cap) {
  if (cap < kHistoryHeaderSize || to < from) return 0;
  size_t n = 0;
  if (!log.forEach(from, to, countVisit, &n)) return 0;
  ExportCtx c;
  memset(&c, 0, sizeof(c));
  c.from = from;
  c.maxRows = maxRows;
  c.out = out;
  c.cap = cap;
  if (n > maxRows) {
    const uint32_t range = to - from + 1;
    c.span = range / static_cast<uint32_t>(maxRows) +
             (range % static_cast<uint32_t>(maxRows) != 0 ? 1 : 0);
  }
  if (!log.forEach(from, to, visit, &c)) return 0;
  flushBucket(&c);
  memcpy(out, "GMH1", 4);
  out[4] = static_cast<uint8_t>(c.rows);
  out[5] = static_cast<uint8_t>(c.rows >> 8);
  out[6] = kMaxPoints;
  out[7] = 0;
  return kHistoryHeaderSize + c.rows * kHistoryRowSize;
}

}  // namespace gm
