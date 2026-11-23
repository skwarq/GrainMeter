// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "ota/multipart.h"
#include "ota/ota_session.h"
#include "support/fake_image.h"

namespace {
// Records what reached the "flash" and whether the boot slot was switched.
class RecordingTarget : public gm::OtaTarget {
 public:
  size_t slotSize() override { return 0x1E0000; }
  const char* slotName() override { return "ota_1"; }
  bool begin(size_t, char*, size_t) override {
    begun = true;
    data.clear();
    return true;
  }
  bool write(const uint8_t* d, size_t n, char* err, size_t cap) override {
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
    bootSwitched = true;
    return true;
  }
  void abort() override { aborted = true; }

  std::vector<uint8_t> data;
  bool begun = false, aborted = false, bootSwitched = false, failWrite = false,
       failFinish = false;
};

bool feedAll(gm::OtaSession& s, const std::vector<uint8_t>& img, size_t chunk) {
  for (size_t off = 0; off < img.size(); off += chunk) {
    const size_t n = std::min(chunk, img.size() - off);
    if (!s.feed(img.data() + off, n)) return false;
  }
  return true;
}
}  // namespace

TEST(OtaSession, AcceptsAnyFileNameAndWritesImage) {
  RecordingTarget t;
  gm::OtaSession s(t, 0x000D, "grainmeter");
  const auto img = fakeImage(70000);
  ASSERT_TRUE(s.start(img.size(), true, "firmware-copy (2).bin"));
  ASSERT_TRUE(feedAll(s, img, 1000));
  ASSERT_TRUE(s.finish());
  EXPECT_TRUE(t.bootSwitched);
  EXPECT_EQ(t.data, img);
  EXPECT_STREQ(s.image().version, "9.9.9-test");
  EXPECT_EQ(s.state(), gm::OtaSession::State::Done);
}

TEST(OtaSession, RejectsBeforeWritingAnything) {
  struct Case {
    std::vector<uint8_t> img;
    int status;
  };
  const Case cases[] = {
      {fakeImage(8000, 0x0002), 415},  // ESP32-S2 image
      {fakeImage(8000, 0x000D, "other_project"), 415},
      {fakeImage(8000, 0x000D, "grainmeter", "1", 0x00), 415},  // bad magic
  };
  for (const Case& c : cases) {
    RecordingTarget t;
    gm::OtaSession s(t, 0x000D, "grainmeter");
    ASSERT_TRUE(s.start(c.img.size(), true, "x.bin"));
    EXPECT_FALSE(feedAll(s, c.img, 4096));
    EXPECT_EQ(s.httpStatus(), c.status);
    EXPECT_FALSE(t.begun);  // slot not even erased
    EXPECT_FALSE(t.bootSwitched);
  }
}

TEST(OtaSession, SizeChecks) {
  RecordingTarget t;
  gm::OtaSession s(t, 0x000D, "grainmeter");
  EXPECT_FALSE(s.start(0, true, "a.bin"));
  EXPECT_EQ(s.httpStatus(), 411);
  EXPECT_FALSE(s.start(100, true, "a.bin"));
  EXPECT_EQ(s.httpStatus(), 400);
  EXPECT_FALSE(s.start(0x1E0001, true, "a.bin"));
  EXPECT_EQ(s.httpStatus(), 413);
}

TEST(OtaSession, InterruptedTransferKeepsBootPartition) {
  RecordingTarget t;
  gm::OtaSession s(t, 0x000D, "grainmeter");
  const auto img = fakeImage(50000);
  ASSERT_TRUE(s.start(img.size(), true, "a.bin"));
  ASSERT_TRUE(s.feed(img.data(), 20000));
  s.abort("client disconnected");
  EXPECT_TRUE(t.aborted);
  EXPECT_FALSE(t.bootSwitched);
  EXPECT_FALSE(s.finish());  // nothing to finish
  // A new upload can start afterwards.
  EXPECT_TRUE(s.start(img.size(), true, "a.bin"));
}

TEST(OtaSession, IncompleteAndFailedVerification) {
  RecordingTarget t;
  gm::OtaSession s(t, 0x000D, "grainmeter");
  const auto img = fakeImage(50000);
  ASSERT_TRUE(s.start(img.size(), true, "a.bin"));
  ASSERT_TRUE(s.feed(img.data(), 40000));
  EXPECT_FALSE(s.finish());
  EXPECT_TRUE(t.aborted);
  EXPECT_FALSE(t.bootSwitched);

  RecordingTarget t2;
  t2.failFinish = true;
  gm::OtaSession s2(t2, 0x000D, "grainmeter");
  ASSERT_TRUE(s2.start(img.size(), true, "a.bin"));
  ASSERT_TRUE(feedAll(s2, img, 3000));
  EXPECT_FALSE(s2.finish());
  EXPECT_EQ(s2.httpStatus(), 422);
  EXPECT_FALSE(t2.bootSwitched);
}

TEST(OtaSession, FlashWriteError) {
  RecordingTarget t;
  t.failWrite = true;
  gm::OtaSession s(t, 0x000D, "grainmeter");
  const auto img = fakeImage(10000);
  ASSERT_TRUE(s.start(img.size(), true, "a.bin"));
  EXPECT_FALSE(feedAll(s, img, 1000));
  EXPECT_EQ(s.httpStatus(), 500);
  EXPECT_TRUE(t.aborted);
}

TEST(OtaSession, SecondUploadWhileBusyIsRefused) {
  RecordingTarget t;
  gm::OtaSession s(t, 0x000D, "grainmeter");
  ASSERT_TRUE(s.start(10000, true, "a.bin"));
  EXPECT_FALSE(s.start(10000, true, "b.bin"));
  EXPECT_EQ(s.httpStatus(), 409);
  EXPECT_TRUE(s.busy());
}

// ---- multipart/form-data (HTML file input) --------------------------------

namespace {
struct Collector {
  std::string data;
  static bool sink(void* ctx, const uint8_t* d, size_t n) {
    static_cast<Collector*>(ctx)->data.append(reinterpret_cast<const char*>(d),
                                              n);
    return true;
  }
};
}  // namespace

TEST(Multipart, ExtractsFileForEveryChunkSplit) {
  std::vector<uint8_t> file(3000);
  for (size_t i = 0; i < file.size(); ++i)
    file[i] = static_cast<uint8_t>(i * 7);
  // Data that looks like the start of a delimiter must pass through.
  const std::string tricky = "\r\n--WebKitFormBoundaryAB\r\n-";
  file.insert(file.begin() + 100, tricky.begin(), tricky.end());
  const std::string body =
      multipartBody("WebKitFormBoundaryABC", file, "grain meter.bin");
  for (size_t chunk : {1u, 2u, 3u, 7u, 64u, 1000u, 100000u}) {
    Collector c;
    gm::MultipartReader r;
    ASSERT_TRUE(r.begin("multipart/form-data; boundary=WebKitFormBoundaryABC",
                        Collector::sink, &c));
    for (size_t off = 0; off < body.size(); off += chunk) {
      ASSERT_TRUE(r.feed(reinterpret_cast<const uint8_t*>(body.data()) + off,
                         std::min(chunk, body.size() - off)))
          << "chunk " << chunk;
    }
    EXPECT_TRUE(r.fileComplete());
    EXPECT_STREQ(r.fileName(), "grain meter.bin");
    EXPECT_EQ(c.data, std::string(file.begin(), file.end()))
        << "chunk " << chunk;
  }
}

TEST(Multipart, QuotedBoundaryAndErrors) {
  Collector c;
  gm::MultipartReader r;
  EXPECT_TRUE(
      r.begin("multipart/form-data; boundary=\"abc\"", Collector::sink, &c));
  EXPECT_FALSE(r.begin("application/octet-stream", Collector::sink, &c));
  EXPECT_FALSE(r.begin("multipart/form-data", Collector::sink, &c));
  ASSERT_TRUE(
      r.begin("multipart/form-data; boundary=abc", Collector::sink, &c));
  const std::string bad =
      "--abc\r\nContent-Disposition: form-data; name=\"f\"; "
      "filename=\"a\"\r\n\r\nDATA";
  EXPECT_TRUE(r.feed(reinterpret_cast<const uint8_t*>(bad.data()), bad.size()));
  EXPECT_FALSE(r.fileComplete());  // truncated upload is detected
}
