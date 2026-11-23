// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "data/retained_state.h"

namespace {
gm::Sample sample(int16_t t) {
  gm::Sample s;
  std::memset(&s, 0, sizeof(s));
  s.epoch = 1700000000u;
  for (int p = 0; p < gm::kMaxPoints; ++p) {
    s.centi[p] = t;
    s.status[p] = 0;
  }
  return s;
}
}  // namespace

class RetainedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    st.reset(new gm::RetainedState());
    st->init();
  }
  std::unique_ptr<gm::RetainedState> st;
};

TEST_F(RetainedTest, GarbageIsInvalid) {
  std::memset(st.get(), 0xA5, sizeof(gm::RetainedState));
  EXPECT_FALSE(st->valid());
  st->init();
  EXPECT_TRUE(st->valid());
  EXPECT_EQ(st->count, 0);
  EXPECT_EQ(st->nextSeq, 1u);
}

TEST_F(RetainedTest, CorruptionDetected) {
  st->push(sample(100));
  st->seal();
  ASSERT_TRUE(st->valid());
  reinterpret_cast<uint8_t*>(st.get())[100] ^= 0x10;
  EXPECT_FALSE(st->valid());
}

TEST_F(RetainedTest, SequenceNumbersIncrease) {
  EXPECT_EQ(st->push(sample(1))->seq, 1u);
  EXPECT_EQ(st->push(sample(2))->seq, 2u);
  EXPECT_EQ(st->at(0).seq, 1u);
  EXPECT_EQ(st->at(1).centi[0], 2);
}

TEST_F(RetainedTest, OverflowDropsOldest) {
  for (int i = 0; i < gm::kBufferCapacity + 5; ++i) {
    st->push(sample(static_cast<int16_t>(i)));
  }
  EXPECT_EQ(st->count, gm::kBufferCapacity);
  EXPECT_EQ(st->droppedOverflow, 5u);
  EXPECT_EQ(st->at(0).seq, 6u);  // 1..5 dropped
  EXPECT_EQ(st->at(gm::kBufferCapacity - 1).seq,
            static_cast<uint32_t>(gm::kBufferCapacity + 5));
}

// Failed upload: nothing removed. Confirmed upload of the first N: exactly
// those removed, newer samples (taken during the upload) stay.
TEST_F(RetainedTest, RemoveOnlyConfirmed) {
  for (int i = 0; i < 6; ++i) st->push(sample(0));
  EXPECT_EQ(st->removeUpTo(0), 0u);
  EXPECT_EQ(st->count, 6);
  EXPECT_EQ(st->removeUpTo(4), 4u);
  EXPECT_EQ(st->count, 2);
  EXPECT_EQ(st->at(0).seq, 5u);
  EXPECT_EQ(st->removeUpTo(4), 0u);  // repeated confirmation is harmless
}

TEST_F(RetainedTest, RemoveAtWrapsAround) {
  for (int i = 0; i < gm::kBufferCapacity + 2; ++i) st->push(sample(0));
  const uint32_t keep = st->at(2).seq;
  st->removeAt(1);
  EXPECT_EQ(st->count, gm::kBufferCapacity - 1);
  EXPECT_EQ(st->at(1).seq, keep);
}
