// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "comm/dns_reply.h"

namespace {
std::vector<uint8_t> query(const char* name, uint16_t qtype) {
  std::vector<uint8_t> q = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
  const char* p = name;
  while (*p) {
    const char* dot = std::strchr(p, '.');
    const size_t n = dot ? static_cast<size_t>(dot - p) : std::strlen(p);
    q.push_back(static_cast<uint8_t>(n));
    q.insert(q.end(), p, p + n);
    p += n + (dot ? 1 : 0);
  }
  q.push_back(0);
  q.push_back(static_cast<uint8_t>(qtype >> 8));
  q.push_back(static_cast<uint8_t>(qtype));
  q.push_back(0);
  q.push_back(1);
  return q;
}
const uint8_t kIp[4] = {192, 168, 4, 1};
uint32_t ip() {
  uint32_t v;
  std::memcpy(&v, kIp, 4);
  return v;
}
}  // namespace

TEST(DnsReply, AnswersAQueryWithPortalAddress) {
  const auto q = query("connectivitycheck.gstatic.com", 1);
  uint8_t out[600];
  const int n = gm::dnsBuildReply(q.data(), static_cast<int>(q.size()), ip(),
                                  out, sizeof(out));
  ASSERT_EQ(n, static_cast<int>(q.size()) + 16);
  EXPECT_EQ(out[0], 0x12);         // same transaction id
  EXPECT_EQ(out[2] & 0x80, 0x80);  // response
  EXPECT_EQ(out[7], 1);            // one answer
  EXPECT_EQ(std::memcmp(out + n - 4, kIp, 4), 0);
}

TEST(DnsReply, NoAnswerForAaaa) {
  const auto q = query("example.com", 28);
  uint8_t out[600];
  const int n = gm::dnsBuildReply(q.data(), static_cast<int>(q.size()), ip(),
                                  out, sizeof(out));
  ASSERT_EQ(n, static_cast<int>(q.size()));
  EXPECT_EQ(out[7], 0);
}

TEST(DnsReply, IgnoresMalformedPackets) {
  uint8_t out[600];
  auto q = query("example.com", 1);
  // truncated anywhere inside the question
  for (size_t len = 0; len < q.size(); ++len) {
    EXPECT_EQ(gm::dnsBuildReply(q.data(), static_cast<int>(len), ip(), out,
                                sizeof(out)),
              0)
        << len;
  }
  auto bad = q;
  bad[12] = 60;  // label longer than the packet
  EXPECT_EQ(gm::dnsBuildReply(bad.data(), static_cast<int>(bad.size()), ip(),
                              out, sizeof(out)),
            0);
  auto resp = q;
  resp[2] |= 0x80;  // already a response
  EXPECT_EQ(gm::dnsBuildReply(resp.data(), static_cast<int>(resp.size()), ip(),
                              out, sizeof(out)),
            0);
  auto two = q;
  two[5] = 2;  // two questions
  EXPECT_EQ(gm::dnsBuildReply(two.data(), static_cast<int>(two.size()), ip(),
                              out, sizeof(out)),
            0);
}
