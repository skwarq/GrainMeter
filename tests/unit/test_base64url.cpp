// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "util/base64url.h"

TEST(Base64url, DecodesBothAlphabetsPaddingAndInPlace) {
  struct Case {
    const char* text;
    std::string bytes;
  };
  const Case cases[] = {{"", ""},
                        {"Zg", "f"},
                        {"Zg==", "f"},
                        {"Zm8", "fo"},
                        {"Zm8=", "fo"},
                        {"Zm9v", "foo"},
                        {"AAEC", std::string("\0\1\2", 3)},
                        {"-_8", std::string("\xfb\xff", 2)},
                        {"+/8=", std::string("\xfb\xff", 2)}};
  for (const auto& c : cases) {
    char buffer[32];
    std::strcpy(buffer, c.text);
    const int len =
        gm::base64urlDecode(buffer, std::strlen(buffer),
                            reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
    ASSERT_EQ(len, static_cast<int>(c.bytes.size())) << c.text;
    EXPECT_EQ(std::string(buffer, static_cast<size_t>(len)), c.bytes);
  }
}

TEST(Base64url, RejectsMalformedInputAndInsufficientCapacityWithoutWriting) {
  const char* cases[] = {"A",   "Zg=",  "Zg===", "Zm9v=", "Z=g=", "Zg==x",
                         "Z g", "Zg\n", "Zh",    "Zm9",   "####"};
  for (const char* text : cases) {
    uint8_t out[8] = {42};
    EXPECT_EQ(gm::base64urlDecode(text, std::strlen(text), out, sizeof(out)),
              -1)
        << text;
    EXPECT_EQ(out[0], 42);
  }
  uint8_t out[2] = {42, 42};
  EXPECT_EQ(gm::base64urlDecode("Zm9v", 4, out, sizeof(out)), -1);
  EXPECT_EQ(out[0], 42);
  EXPECT_EQ(gm::base64urlDecode(nullptr, 1, out, sizeof(out)), -1);
  EXPECT_EQ(gm::base64urlDecode("Zg", 2, nullptr, 1), -1);
}
