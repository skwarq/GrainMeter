// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "ota/ota_upload.h"
#include "sim/sim_clock.h"
#include "support/fake_image.h"

namespace {
class UploadTarget : public gm::OtaTarget {
 public:
  size_t slotSize() override { return 100000; }
  const char* slotName() override { return "ota_1"; }
  bool begin(size_t, char*, size_t) override {
    begun = true;
    data.clear();
    return true;
  }
  bool write(const uint8_t* d, size_t n, char* err, size_t cap) override {
    ++writes;
    if (failWrite) {
      std::snprintf(err, cap, "flash write failed");
      return false;
    }
    data.insert(data.end(), d, d + n);
    return true;
  }
  bool finish(char* err, size_t cap) override {
    if (failFinish) {
      std::snprintf(err, cap, "image verification failed");
      return false;
    }
    selected = true;
    return true;
  }
  void abort() override { aborted = true; }
  std::vector<uint8_t> data;
  unsigned writes = 0;
  bool begun = false, selected = false, aborted = false;
  bool failWrite = false, failFinish = false;
};

class OtaUploadTest : public ::testing::Test {
 protected:
  UploadTarget target;
  gm::OtaSession session{target, gm::kChipIdEsp32C6, "grainmeter"};
  gmpc::SimClock clock{1, true};
  gm::OtaUpload upload{session, clock};
  std::vector<uint8_t> image = fakeImage(70001);
  char hash[65];
  void SetUp() override { hashImage(); }
  void hashImage() {
    gm::Sha256 sha;
    uint8_t digest[32];
    sha.update(image.data(), image.size());
    sha.finish(digest);
    gm::sha256ToHex(digest, hash);
  }
  void begin() {
    ASSERT_TRUE(upload.begin(image.size(), hash, "test.bin", false));
  }
  void sendAll(size_t off = 0) {
    while (off < image.size()) {
      const size_t len =
          std::min<size_t>(gm::OtaUpload::kChunkSize, image.size() - off);
      ASSERT_TRUE(upload.chunk(off, image.data() + off, len));
      off += len;
    }
  }
};

TEST_F(OtaUploadTest, WholeImageDuplicateGapAndPartialOverlap) {
  begin();
  ASSERT_TRUE(upload.chunk(0, image.data(), 1440));
  const unsigned writes = target.writes;
  EXPECT_TRUE(upload.chunk(0, image.data(), 1440));
  EXPECT_EQ(target.writes, writes);
  EXPECT_EQ(upload.next(), 1440u);
  EXPECT_FALSE(upload.chunk(2880, image.data() + 2880, 1440));
  EXPECT_EQ(upload.httpStatus(), 409);
  EXPECT_STREQ(upload.error(), "offset");
  EXPECT_EQ(upload.next(), 1440u);
  EXPECT_FALSE(upload.chunk(1000, image.data() + 1000, 1440));
  EXPECT_EQ(upload.httpStatus(), 409);
  EXPECT_EQ(target.writes, writes);
  sendAll(1440);
  ASSERT_TRUE(upload.finish(hash));
  EXPECT_TRUE(target.selected);
  EXPECT_EQ(target.data, image);
  EXPECT_FALSE(upload.active());
}

TEST_F(OtaUploadTest, BadDigestAtBeginOrFinishAndAlteredImage) {
  EXPECT_FALSE(upload.begin(image.size(), "bad", "test.bin", false));
  EXPECT_EQ(upload.httpStatus(), 400);
  EXPECT_FALSE(session.busy());
  begin();
  sendAll();
  hash[0] = hash[0] == '0' ? '1' : '0';
  EXPECT_FALSE(upload.finish(hash));
  EXPECT_EQ(upload.httpStatus(), 422);
  EXPECT_STREQ(upload.error(), "sha256");
  EXPECT_TRUE(target.aborted);
  EXPECT_FALSE(target.selected);

  hashImage();
  begin();
  image.back() ^= 1;
  sendAll();
  EXPECT_FALSE(upload.finish(hash));
  EXPECT_EQ(upload.httpStatus(), 422);
  EXPECT_FALSE(target.selected);
}

TEST_F(OtaUploadTest, AbortAndTimeoutReleaseTheSession) {
  begin();
  ASSERT_TRUE(upload.chunk(0, image.data(), 1440));
  upload.abort();
  upload.abort();
  EXPECT_FALSE(session.busy());
  EXPECT_TRUE(target.aborted);
  EXPECT_FALSE(target.selected);
  begin();
  clock.advanceMs(119000);
  upload.tick();
  EXPECT_TRUE(upload.active());
  ASSERT_TRUE(upload.chunk(0, image.data(), 1440));
  clock.advanceMs(119000);
  upload.tick();
  EXPECT_TRUE(upload.active());
  clock.advanceMs(1000);
  upload.tick();
  EXPECT_FALSE(upload.active());
  EXPECT_FALSE(session.busy());
  EXPECT_STREQ(session.error(), "upload timeout");
  EXPECT_FALSE(upload.finish(hash));
  EXPECT_EQ(upload.httpStatus(), 409);
  begin();
  clock.advanceMs(120000);
  EXPECT_FALSE(upload.chunk(0, image.data(), 1440));
  EXPECT_FALSE(session.busy());
}

TEST_F(OtaUploadTest, TimeoutHandlesMonotonicWrap) {
  clock.advanceMs(static_cast<uint64_t>(UINT32_MAX - 60u) * 1000u);
  begin();
  clock.advanceMs(119000);
  upload.tick();
  EXPECT_TRUE(upload.active());
  clock.advanceMs(1000);
  upload.tick();
  EXPECT_FALSE(upload.active());
}

TEST_F(OtaUploadTest, HttpExclusionAndAbortDoesNotTouchHttp) {
  ASSERT_TRUE(session.start(image.size(), true, "http.bin"));
  EXPECT_FALSE(upload.begin(image.size(), hash, "test.bin", false));
  EXPECT_EQ(upload.httpStatus(), 409);
  EXPECT_STREQ(upload.error(), "busy");
  EXPECT_FALSE(upload.chunk(0, image.data(), 1440));
  EXPECT_FALSE(upload.finish(hash));
  upload.abort();
  clock.advanceMs(120000);
  upload.tick();
  EXPECT_TRUE(session.busy());
  EXPECT_EQ(session.received(), 0u);
  session.abort("http aborted");
  begin();
  EXPECT_FALSE(session.start(image.size(), true, "http.bin"));
  EXPECT_TRUE(upload.active());
  sendAll();
  EXPECT_TRUE(upload.finish(hash));
}

TEST_F(OtaUploadTest, CriticalSizeAndBeginPolicyRefusals) {
  EXPECT_FALSE(upload.begin(image.size(), hash, "test.bin", true));
  EXPECT_EQ(upload.httpStatus(), 503);
  EXPECT_STREQ(upload.error(), "low_battery");
  EXPECT_FALSE(upload.begin(target.slotSize() + 1, hash, "test.bin", false));
  EXPECT_EQ(upload.httpStatus(), 413);
  EXPECT_FALSE(target.begun);
  unsigned calls = 0;
  upload.setCanBegin(
      [](void* ctx, int* status, const char** error) {
        ++*static_cast<unsigned*>(ctx);
        *status = 409;
        *error = "session_short";
        return false;
      },
      &calls);
  EXPECT_FALSE(upload.begin(image.size(), hash, "test.bin", false));
  EXPECT_EQ(upload.httpStatus(), 409);
  EXPECT_STREQ(upload.error(), "session_short");
  EXPECT_EQ(calls, 1u);
  EXPECT_FALSE(session.busy());
  EXPECT_FALSE(target.begun);
}

TEST_F(OtaUploadTest, InvalidHeaderNeverErasesOrWritesTarget) {
  image[0] = 0;
  hashImage();
  begin();
  EXPECT_FALSE(upload.chunk(0, image.data(), 1440));
  EXPECT_EQ(upload.httpStatus(), 415);
  EXPECT_FALSE(target.begun);
  EXPECT_TRUE(target.data.empty());
  EXPECT_FALSE(upload.active());
}

TEST_F(OtaUploadTest, InvalidChunkAndImageFailures) {
  begin();
  EXPECT_FALSE(upload.chunk(0, image.data(), 1441));
  EXPECT_EQ(upload.httpStatus(), 400);
  EXPECT_FALSE(upload.chunk(0, image.data(), 0));
  EXPECT_EQ(session.received(), 0u);
  EXPECT_TRUE(upload.active());
  target.failWrite = true;
  EXPECT_FALSE(upload.chunk(0, image.data(), 1440));
  EXPECT_EQ(upload.httpStatus(), 500);
  EXPECT_TRUE(target.aborted);
  EXPECT_FALSE(upload.active());
  target.failWrite = false;
  begin();
  sendAll();
  target.failFinish = true;
  EXPECT_FALSE(upload.finish(hash));
  EXPECT_EQ(upload.httpStatus(), 422);
  EXPECT_FALSE(target.selected);
}
}  // namespace
