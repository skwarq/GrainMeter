// SPDX-License-Identifier: MIT
#include "data/history_log.h"

#include <string.h>

#include "util/crc.h"
#include "util/log.h"

namespace gm {

static const char* kTag = "history";

// Format v3 (variable length). Older formats (0.2.0: 32-byte records
// starting with "GH" or 0xA2) are not recognised: their sectors are erased
// as the log reaches them.
//
// key   (34 B): B3 len seq32 epoch32 centi[8]x16 status[4] bat rssi flags crc
// delta (6+ B): D3 len zz(dt) pmask zz(dcenti)... emask [status4][bat][rssi]
//               [flags] crc
// zz = zigzag varint (LEB128). crc = CRC-8 of all preceding bytes.
static const uint8_t kTypeKey = 0xB3;
static const uint8_t kTypeDelta = 0xD3;
enum { kEmStatus = 1, kEmBattery = 2, kEmRssi = 4, kEmFlags = 8 };
static const uint8_t kStoredFlagsMask =
    static_cast<uint8_t>(~(kHistHold & 0xFF));

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

static uint8_t batteryCode(uint16_t mv) {
  if (mv == 0) return 0xFF;
  if (mv <= 2000) return 0;
  const unsigned c = (mv - 2000u + 5u) / 10u;
  return static_cast<uint8_t>(c > 254 ? 254 : c);
}

static uint16_t batteryMv(uint8_t code) {
  return code == 0xFF ? 0 : static_cast<uint16_t>(2000u + code * 10u);
}

static size_t putVar(uint8_t* p, int32_t v) {
  uint32_t z = (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
  size_t n = 0;
  while (z >= 0x80) {
    p[n++] = static_cast<uint8_t>(z | 0x80);
    z >>= 7;
  }
  p[n++] = static_cast<uint8_t>(z);
  return n;
}

// Returns false if the varint runs past end.
static bool getVar(const uint8_t* p, size_t end, size_t* pos, int32_t* v) {
  uint32_t z = 0;
  for (int shift = 0; shift < 35; shift += 7) {
    if (*pos >= end) return false;
    const uint8_t b = p[(*pos)++];
    z |= static_cast<uint32_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      *v = static_cast<int32_t>((z >> 1) ^ (~(z & 1) + 1));
      return true;
    }
  }
  return false;
}

static void packStatus(const uint8_t s[kMaxPoints], uint8_t* out) {
  for (int i = 0; i < kMaxPoints / 2; ++i) {
    out[i] =
        static_cast<uint8_t>((s[2 * i] & 0x0F) | ((s[2 * i + 1] & 0x0F) << 4));
  }
}

static void unpackStatus(const uint8_t* in, uint8_t s[kMaxPoints]) {
  for (int i = 0; i < kMaxPoints / 2; ++i) {
    s[2 * i] = static_cast<uint8_t>(in[i] & 0x0F);
    s[2 * i + 1] = static_cast<uint8_t>(in[i] >> 4);
  }
}

size_t HistoryLog::encodeKey(const HistoryRecord& r, uint32_t sectorSeq,
                             uint8_t* out) {
  out[0] = kTypeKey;
  out[1] = kHistoryKeySize;
  put32(out + 2, sectorSeq);
  put32(out + 6, r.epoch);
  for (int i = 0; i < kMaxPoints; ++i) {
    const uint16_t v = static_cast<uint16_t>(r.centi[i]);
    out[10 + 2 * i] = static_cast<uint8_t>(v);
    out[11 + 2 * i] = static_cast<uint8_t>(v >> 8);
  }
  packStatus(r.status, out + 26);
  out[30] = batteryCode(r.batteryMv);
  out[31] = static_cast<uint8_t>(r.rssi);
  out[32] = r.flags & kStoredFlagsMask;
  out[33] = crc8Maxim(out, kHistoryKeySize - 1);
  return kHistoryKeySize;
}

size_t HistoryLog::encodeDelta(const HistoryRecord& base,
                               const HistoryRecord& r, uint8_t* out) {
  size_t n = 2;
  n += putVar(out + n, static_cast<int32_t>(r.epoch - base.epoch));
  const size_t maskPos = n++;
  uint8_t pmask = 0;
  for (int i = 0; i < kMaxPoints; ++i) {
    if (r.centi[i] != base.centi[i]) {
      pmask = static_cast<uint8_t>(pmask | (1u << i));
      n += putVar(out + n, static_cast<int32_t>(r.centi[i]) - base.centi[i]);
    }
  }
  out[maskPos] = pmask;
  const size_t emPos = n++;
  uint8_t em = 0;
  if (memcmp(r.status, base.status, sizeof(r.status)) != 0) {
    em |= kEmStatus;
    packStatus(r.status, out + n);
    n += kMaxPoints / 2;
  }
  if (batteryCode(r.batteryMv) != batteryCode(base.batteryMv)) {
    em |= kEmBattery;
    out[n++] = batteryCode(r.batteryMv);
  }
  if (r.rssi != base.rssi) {
    em |= kEmRssi;
    out[n++] = static_cast<uint8_t>(r.rssi);
  }
  if ((r.flags & kStoredFlagsMask) != (base.flags & kStoredFlagsMask)) {
    em |= kEmFlags;
    out[n++] = r.flags & kStoredFlagsMask;
  }
  out[emPos] = em;
  out[0] = kTypeDelta;
  out[1] = static_cast<uint8_t>(n + 1);
  out[n] = crc8Maxim(out, n);
  return n + 1;
}

size_t HistoryLog::decode(const uint8_t* in, size_t avail, HistoryRecord* state,
                          uint32_t* sectorSeq, bool* valid) {
  *valid = false;
  if (avail < 2 || (in[0] != kTypeKey && in[0] != kTypeDelta)) return 0;
  const size_t len = in[1];
  if (len < 6 || len > kHistoryMaxRecord || len > avail ||
      (in[0] == kTypeKey && len != kHistoryKeySize)) {
    return 0;
  }
  if (in[len - 1] != crc8Maxim(in, len - 1)) return len;
  const size_t end = len - 1;
  if (in[0] == kTypeKey) {
    *sectorSeq = get32(in + 2);
    state->epoch = get32(in + 6);
    for (int i = 0; i < kMaxPoints; ++i) {
      state->centi[i] = static_cast<int16_t>(
          static_cast<uint16_t>(in[10 + 2 * i] | (in[11 + 2 * i] << 8)));
    }
    unpackStatus(in + 26, state->status);
    state->batteryMv = batteryMv(in[30]);
    state->rssi = static_cast<int8_t>(in[31]);
    state->flags = in[32];
    *valid = true;
    return len;
  }
  HistoryRecord s = *state;
  size_t pos = 2;
  int32_t v = 0;
  if (!getVar(in, end, &pos, &v)) return len;
  s.epoch = state->epoch + static_cast<uint32_t>(v);
  if (pos >= end) return len;
  const uint8_t pmask = in[pos++];
  for (int i = 0; i < kMaxPoints; ++i) {
    if (pmask & (1u << i)) {
      if (!getVar(in, end, &pos, &v)) return len;
      s.centi[i] = static_cast<int16_t>(s.centi[i] + v);
    }
  }
  if (pos >= end) return len;
  const uint8_t em = in[pos++];
  const size_t need = ((em & kEmStatus) ? kMaxPoints / 2 : 0) +
                      ((em & kEmBattery) ? 1 : 0) + ((em & kEmRssi) ? 1 : 0) +
                      ((em & kEmFlags) ? 1 : 0);
  if (pos + need != end) return len;
  if (em & kEmStatus) {
    unpackStatus(in + pos, s.status);
    pos += kMaxPoints / 2;
  }
  if (em & kEmBattery) s.batteryMv = batteryMv(in[pos++]);
  if (em & kEmRssi) s.rssi = static_cast<int8_t>(in[pos++]);
  if (em & kEmFlags) s.flags = in[pos++];
  *state = s;
  *valid = true;
  return len;
}

// Small read-ahead window: records are a few bytes, flash reads have a
// per-call overhead.
class HistoryLog::Reader {
 public:
  Reader(HistoryStorage* st, size_t end) : st_(st), end_(end) {}
  const uint8_t* at(size_t off, size_t* avail) {
    const size_t maxRec = kHistoryMaxRecord;
    const size_t want = end_ - off < maxRec ? end_ - off : maxRec;
    if (off < off_ || off + want > off_ + len_) {
      off_ = off;
      len_ = end_ - off < sizeof(buf_) ? end_ - off : sizeof(buf_);
      if (!st_->read(off_, buf_, len_)) {
        len_ = 0;
        return nullptr;
      }
    }
    *avail = off_ + len_ - off;
    return buf_ + (off - off_);
  }

 private:
  HistoryStorage* st_;
  size_t end_;
  size_t off_ = 0;
  size_t len_ = 0;
  uint8_t buf_[256];
};

size_t HistoryLog::sectors() const {
  return st_ != nullptr ? st_->size() / st_->sectorSize() : 0;
}

bool HistoryLog::keyAt(size_t sector, HistoryRecord* r, uint32_t* seq) {
  uint8_t buf[kHistoryKeySize];
  if (!st_->read(sector * st_->sectorSize(), buf, sizeof(buf))) return false;
  bool valid = false;
  return buf[0] == kTypeKey && decode(buf, sizeof(buf), r, seq, &valid) &&
         valid;
}

// Replays one sector from its key record. endOff = where the next record
// goes (sector end if the rest is not blank).
bool HistoryLog::replay(size_t sector, Visitor fn, void* ctx, size_t* endOff,
                        size_t* records, bool* stopped) {
  const size_t sec = st_->sectorSize();
  const size_t start = sector * sec;
  const size_t end = start + sec;
  Reader rd(st_, end);
  HistoryRecord state = {};
  uint32_t seq = 0;
  size_t off = start;
  *endOff = end;
  while (off < end) {
    size_t avail = 0;
    const uint8_t* p = rd.at(off, &avail);
    if (p == nullptr) return false;
    if (p[0] == 0xFF) {
      // Free space must be blank to be written (a torn write may have left
      // bytes further on).
      for (size_t o = off; o < end;) {
        const uint8_t* q = rd.at(o, &avail);
        if (q == nullptr) return false;
        for (size_t i = 0; i < avail; ++i) {
          if (q[i] != 0xFF) return true;  // unusable rest: endOff = end
        }
        o += avail;
      }
      *endOff = off;
      return true;
    }
    bool valid = false;
    const size_t len = decode(p, avail, &state, &seq, &valid);
    if (len == 0) return true;  // garbage: rest of the sector unusable
    if (valid && (off != start) == (p[0] == kTypeKey)) valid = false;
    if (off == start && !valid) return true;  // no key: sector unusable
    off += len;
    if (valid) {
      if (records != nullptr) ++*records;
      if (fn != nullptr && !fn(ctx, state)) {
        *stopped = true;
        return true;
      }
    }
  }
  return true;
}

static bool copyLast(void* ctx, const HistoryRecord& r) {
  *static_cast<HistoryRecord*>(ctx) = r;
  return true;
}

bool HistoryLog::begin(HistoryStorage* storage) {
  st_ = storage;
  ok_ = false;
  hasBase_ = false;
  needKey_ = false;
  if (st_ == nullptr || st_->sectorSize() < 4 * kHistoryMaxRecord ||
      sectors() < 2) {
    return false;
  }
  const size_t sec = st_->sectorSize();
  size_t newest = 0;
  uint32_t newestSeq = 0;
  bool any = false;
  for (size_t s = 0; s < sectors(); ++s) {
    HistoryRecord r;
    uint32_t seq = 0;
    if (keyAt(s, &r, &seq) && (!any || seq > newestSeq)) {
      any = true;
      newest = s;
      newestSeq = seq;
    }
  }
  if (!any) {
    next_ = 0;  // empty log: the first append erases sector 0
    nextSectorSeq_ = 1;
  } else {
    size_t endOff = 0;
    bool stopped = false;
    if (!replay(newest, copyLast, &base_, &endOff, nullptr, &stopped)) {
      GM_LOGE(kTag, "read failed");
      return false;
    }
    hasBase_ = true;
    next_ = endOff == (newest + 1) * sec ? endOff % st_->size() : endOff;
    nextSectorSeq_ = newestSeq + 1;
  }
  ok_ = true;
  return true;
}

bool HistoryLog::last(HistoryRecord* r) const {
  if (!ok_ || !hasBase_) return false;
  *r = base_;
  return true;
}

static uint32_t absDiff(int32_t a, int32_t b) {
  return static_cast<uint32_t>(a > b ? a - b : b - a);
}

// 0 = within the dead bands, 1 = only the heartbeat is due, 2 = changed.
int HistoryLog::changed(const HistoryRecord& r) const {
  const HistoryRecord& b = base_;
  if (r.epoch < b.epoch) return 2;
  const int idle = r.epoch - b.epoch >= policy_.heartbeatS ? 1 : 0;
  if (memcmp(r.status, b.status, sizeof(r.status)) != 0) return 2;
  for (int i = 0; i < kMaxPoints; ++i) {
    const bool rm = r.centi[i] == kHistoryMissing;
    const bool bm = b.centi[i] == kHistoryMissing;
    if (rm != bm) return 2;
    if (!rm && absDiff(r.centi[i], b.centi[i]) >= policy_.deadbandCenti) {
      return 2;
    }
  }
  if ((r.batteryMv == 0) != (b.batteryMv == 0) ||
      absDiff(r.batteryMv, b.batteryMv) >= policy_.batteryDeadbandMv) {
    return 2;
  }
  if ((r.rssi == 0) != (b.rssi == 0) ||
      absDiff(r.rssi, b.rssi) >= policy_.rssiDeadbandDb) {
    return 2;
  }
  const uint8_t m = static_cast<uint8_t>(kStoredFlagsMask & ~kHistRestart);
  return (r.flags & m) != (b.flags & m) ? 2 : idle;
}

bool HistoryLog::write(const HistoryRecord& r) {
  const size_t sec = st_->sectorSize();
  uint8_t buf[kHistoryMaxRecord];
  size_t len = 0;
  bool key = !hasBase_ || needKey_ || next_ % sec == 0;
  if (!key) {
    len = encodeDelta(base_, r, buf);
    if (next_ % sec + len > sec) key = true;
  }
  if (key) {
    if (next_ % sec != 0) next_ = (next_ / sec + 1) * sec % st_->size();
    len = encodeKey(r, nextSectorSeq_++, buf);
    needKey_ = true;  // until the key is on flash
    if (!st_->eraseSector(next_)) {
      GM_LOGE(kTag, "erase failed at 0x%lx", static_cast<unsigned long>(next_));
      next_ = (next_ + sec) % st_->size();
      return false;
    }
  }
  bool ok = st_->write(next_, buf, len);
  uint8_t check[kHistoryMaxRecord];
  ok = ok && st_->read(next_, check, len) && memcmp(buf, check, len) == 0;
  // A failed record is skipped either way so that it is never rewritten;
  // the base stays at the last good record (as a reader will see it).
  next_ += len;
  if (next_ % sec == 0) next_ %= st_->size();
  if (!ok) {
    // Readers may stop at (or misread) the bad bytes: later deltas would be
    // lost or decoded against the wrong base, so start a fresh sector.
    needKey_ = true;
    GM_LOGE(kTag, "write failed");
    return false;
  }
  HistoryRecord s = base_;
  uint32_t seq = 0;
  bool valid = false;
  decode(buf, len, &s, &seq, &valid);  // the state exactly as stored
  base_ = s;
  hasBase_ = true;
  if (key) needKey_ = false;
  return true;
}

bool HistoryLog::append(const HistoryRecord& r, uint32_t prevEpoch,
                        bool* stored) {
  if (stored != nullptr) *stored = false;
  if (!ok_) return false;
  const bool restart = (r.flags & kHistRestart) != 0;
  const int why = hasBase_ && !restart ? changed(r) : 2;
  if (why == 0) return true;
  if (hasBase_ && !restart && why == 2 && prevEpoch != 0 &&
      prevEpoch > base_.epoch && prevEpoch < r.epoch &&
      prevEpoch - base_.epoch < policy_.heartbeatS) {
    // End of a plateau: the last unchanged measurement keeps the held state.
    HistoryRecord hold = base_;
    hold.epoch = prevEpoch;
    hold.flags = static_cast<uint8_t>(hold.flags & ~kHistRestart);
    bool holdOk = write(hold);
    if (!write(r)) return false;
    if (stored != nullptr) *stored = true;
    return holdOk;
  }
  if (!write(r)) return false;
  if (stored != nullptr) *stored = true;
  return true;
}

bool HistoryLog::stats(HistoryStats* s) {
  memset(s, 0, sizeof(*s));
  if (!ok_) return false;
  const size_t sec = st_->sectorSize();
  s->capacityBytes = (sectors() - 1) * sec;
  uint32_t oldestSeq = 0;
  for (size_t i = 0; i < sectors(); ++i) {
    HistoryRecord key;
    uint32_t seq = 0;
    if (!keyAt(i, &key, &seq)) continue;
    size_t endOff = 0;
    bool stopped = false;
    if (!replay(i, nullptr, nullptr, &endOff, &s->records, &stopped)) {
      return false;
    }
    s->usedBytes += endOff - i * sec;
    if (s->oldestEpoch == 0 || seq < oldestSeq) {
      oldestSeq = seq;
      s->oldestEpoch = key.epoch;
    }
  }
  if (hasBase_) s->newestEpoch = base_.epoch;
  return true;
}

namespace {
struct RangeCtx {
  uint32_t from;
  uint32_t to;
  HistoryLog::Visitor fn;
  void* ctx;
};

bool inRange(void* c, const HistoryRecord& r) {
  RangeCtx* rc = static_cast<RangeCtx*>(c);
  if (r.epoch < rc->from || r.epoch > rc->to) return true;
  return rc->fn(rc->ctx, r);
}
}  // namespace

bool HistoryLog::forEach(uint32_t from, uint32_t to, Visitor fn, void* ctx) {
  if (!ok_) return false;
  const size_t n = sectors();
  // Sectors are written in ring order: the oldest follows the newest.
  size_t newest = 0;
  uint32_t newestSeq = 0;
  bool any = false;
  for (size_t s = 0; s < n; ++s) {
    HistoryRecord r;
    uint32_t seq = 0;
    if (keyAt(s, &r, &seq) && (!any || seq > newestSeq)) {
      any = true;
      newest = s;
      newestSeq = seq;
    }
  }
  if (!any) return true;
  RangeCtx rc = {from, to, fn, ctx};
  bool started = false;
  uint32_t prevSeq = 0;
  for (size_t k = 1; k <= n; ++k) {
    const size_t s = (newest + k) % n;
    HistoryRecord key;
    uint32_t seq = 0;
    if (!keyAt(s, &key, &seq) || seq > newestSeq) continue;
    // Sequence must grow along the ring; an older sector in between was
    // left by a failed erase and would put old rows out of order.
    if (started && seq <= prevSeq) continue;
    started = true;
    prevSeq = seq;
    if (key.epoch > to) break;
    if (s != newest) {
      // Skip the whole sector if the next one already starts before `from`.
      HistoryRecord nk;
      uint32_t nseq = 0;
      const size_t t = (s + 1) % n;
      if (keyAt(t, &nk, &nseq) && nseq == seq + 1 && nk.epoch <= from) {
        continue;
      }
    }
    size_t endOff = 0;
    bool stopped = false;
    if (!replay(s, inRange, &rc, &endOff, nullptr, &stopped)) return false;
    if (stopped) break;
  }
  return true;
}

// ----------------------------------------------------------------- export --

namespace {
struct ExportCtx {
  const HistoryExportRequest* req;
  uint8_t* out;
  size_t maxRows;
  size_t rows;
  uint32_t next;
  bool haveCarry;
  HistoryRecord carry;
  bool haveLast;
  uint32_t lastEpoch;
};

void putRow(ExportCtx* c, const HistoryRecord& r, uint32_t epoch,
            uint8_t extraFlags) {
  uint8_t* p = c->out + kHistoryHeaderSize + c->rows * kHistoryRowSize;
  put32(p, epoch);
  for (int i = 0; i < kMaxPoints; ++i) {
    const bool ok = r.status[i] == static_cast<uint8_t>(ReadStatus::Ok);
    const int16_t missing = kHistoryMissing;
    const uint16_t v = static_cast<uint16_t>(ok ? r.centi[i] : missing);
    p[4 + 2 * i] = static_cast<uint8_t>(v);
    p[5 + 2 * i] = static_cast<uint8_t>(v >> 8);
  }
  p[20] = static_cast<uint8_t>(r.batteryMv);
  p[21] = static_cast<uint8_t>(r.batteryMv >> 8);
  p[22] = static_cast<uint8_t>(r.rssi);
  p[23] = static_cast<uint8_t>((r.flags & ~kHistHold) | extraFlags);
  ++c->rows;
  c->haveLast = true;
  c->lastEpoch = epoch;
}

// The state at the range start: the last record before it, if recent.
void flushCarry(ExportCtx* c) {
  if (c->haveCarry && c->req->from - c->carry.epoch <= c->req->maxGapS &&
      c->rows < c->maxRows) {
    putRow(c, c->carry, c->carry.epoch, 0);
  }
  c->haveCarry = false;
}

bool exportVisit(void* ctx, const HistoryRecord& r) {
  ExportCtx* c = static_cast<ExportCtx*>(ctx);
  if (r.epoch < c->req->from) {
    c->carry = r;
    c->haveCarry = true;
    return true;
  }
  flushCarry(c);
  if (c->rows >= c->maxRows) {
    c->next = r.epoch;
    return false;
  }
  putRow(c, r, r.epoch, 0);
  return true;
}
}  // namespace

size_t historyExport(HistoryLog& log, const HistoryExportRequest& req,
                     uint8_t* out, size_t cap) {
  if (cap < kHistoryHeaderSize + kHistoryRowSize || req.maxRows == 0) {
    return 0;
  }
  ExportCtx c;
  memset(&c, 0, sizeof(c));
  c.req = &req;
  c.out = out;
  const size_t fit = (cap - kHistoryHeaderSize) / kHistoryRowSize;
  c.maxRows = req.maxRows < fit ? req.maxRows : fit;
  const uint32_t scanFrom = req.from > req.maxGapS ? req.from - req.maxGapS : 0;
  if (!log.forEach(scanFrom, req.to, exportVisit, &c)) return 0;
  flushCarry(&c);
  // The last measurement is usually not stored (unchanged): hold the last
  // stored state until then so the chart reaches "now".
  HistoryRecord last;
  if (c.next == 0 && req.holdUntil >= req.from && req.holdUntil <= req.to &&
      log.last(&last) && c.haveLast && c.lastEpoch == last.epoch &&
      last.epoch < req.holdUntil && req.holdUntil - last.epoch <= req.maxGapS) {
    if (c.rows < c.maxRows) {
      putRow(&c, last, req.holdUntil, kHistHold);
    } else {
      c.next = req.holdUntil;
    }
  }
  memcpy(out, "GMH3", 4);
  out[4] = static_cast<uint8_t>(c.rows);
  out[5] = static_cast<uint8_t>(c.rows >> 8);
  out[6] = kMaxPoints;
  out[7] = 0;
  put32(out + 8, c.next);
  put32(out + 12, req.maxGapS);
  return kHistoryHeaderSize + c.rows * kHistoryRowSize;
}

}  // namespace gm
