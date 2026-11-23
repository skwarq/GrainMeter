// SPDX-License-Identifier: MIT
#ifndef GM_RETAINED_STATE_H
#define GM_RETAINED_STATE_H

#include <stddef.h>
#include <stdint.h>

#include "data/measurement.h"

#ifndef GM_BUFFER_CAPACITY
// 96 samples = 24 h at the default 15 min interval (~3.5 KiB).
#define GM_BUFFER_CAPACITY 96
#endif

namespace gm {

enum { kBufferCapacity = GM_BUFFER_CAPACITY };

struct LastUpload {
  uint32_t epoch;  // wall clock of the attempt (0 = unknown)
  uint32_t monoS;
  uint8_t attempted;
  uint8_t ok;      // main channel write confirmed
  uint8_t diagOk;  // diagnostic channel write confirmed
  uint8_t reserved;
  int16_t httpStatus;  // last HTTP status from main channel, -1 = none
  uint16_t samplesSent;
  uint16_t samplesConfirmed;
  char message[96];  // short human readable outcome (never secrets)
};

// Everything that must survive deep sleep. Plain-old-data so the ESP32 can
// place it in RTC memory (RTC_NOINIT_ATTR) and the PC build can store it in a
// file. Integrity is protected by magic + size + CRC-32; RTC memory after a
// power loss contains garbage and fails validation, so it is re-initialised.
struct RetainedState {
  uint32_t magic;
  uint32_t size;

  // Ring buffer of samples awaiting confirmed upload (oldest first).
  uint16_t head;  // index of oldest sample
  uint16_t count;
  Sample items[kBufferCapacity];

  uint32_t nextSeq;
  uint32_t droppedOverflow;  // oldest samples discarded because full
  uint32_t droppedNoTime;    // samples without a reconstructable time
  uint32_t uploadOkCount;
  uint32_t uploadFailCount;
  uint32_t wakeCount;

  uint8_t hasMeasured;
  uint8_t hasUploadAttempt;
  uint8_t reserved[2];
  uint32_t lastMeasureMonoS;
  uint32_t lastUploadAttemptMonoS;

  LastUpload last;

  uint8_t hasDiag;
  uint8_t hasUpdateCheck;
  uint8_t updateResult;  // RemoteUpdateResult (app/grainmeter.h)
  uint8_t reserved2;
  uint32_t lastDiagMonoS;
  uint32_t lastUpdateCheckMonoS;
  char updateLatest[33];  // version announced by the manifest
  char updateMessage[127];

  uint32_t crc;

  // --- helpers (no virtuals: must stay POD-compatible) ---
  void init();
  bool valid() const;  // magic, size, indices and CRC
  void seal();         // recompute CRC after modification

  const Sample& at(size_t i) const {
    return items[(head + i) % kBufferCapacity];
  }
  Sample& at(size_t i) { return items[(head + i) % kBufferCapacity]; }
  // Appends; when full the oldest sample is dropped (droppedOverflow++).
  // Assigns seq. Returns pointer to the stored sample.
  Sample* push(const Sample& s);
  // Removes all samples with seq <= lastSeq from the front.
  size_t removeUpTo(uint32_t lastSeq);
  // Removes the sample at logical index i (used for time-less samples).
  void removeAt(size_t i);
};

static const uint32_t kRetainedMagic = 0x474D5232u;  // "GMR2" (layout v2)

}  // namespace gm

#endif
