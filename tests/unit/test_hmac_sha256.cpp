// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "support/gm2.h"
#include "util/hmac_sha256.h"
TEST(HmacSha256, Rfc4231) {
  struct Vector {
    std::string key, data;
    const char* expected;
  };
  std::string ascending;
  for (int i = 1; i <= 25; ++i) ascending += static_cast<char>(i);
  const Vector vectors[] = {
      {std::string(20, '\x0b'), "Hi There",
       "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
      {"Jefe", "what do ya want for nothing?",
       "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
      {std::string(20, '\xaa'), std::string(50, '\xdd'),
       "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
      {ascending, std::string(50, '\xcd'),
       "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
      {std::string(131, '\xaa'),
       "Test Using Larger Than Block-Size Key - Hash Key First",
       "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
      {std::string(131, '\xaa'),
       "This is a test using a larger than block-size key and a larger than "
       "block-size data. The key needs to be hashed before being used by the "
       "HMAC algorithm.",
       "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"}};
  for (const auto& v : vectors) {
    char result[65];
    gm::hmacSha256Hex(v.key.data(), v.key.size(), v.data.data(), v.data.size(),
                      result);
    EXPECT_STREQ(result, v.expected);
    gm::HmacSha256 stream(v.key.data(), v.key.size());
    for (char c : v.data) stream.update(&c, 1);
    uint8_t digest[32];
    stream.finish(digest);
    gm::sha256ToHex(digest, result);
    EXPECT_STREQ(result, v.expected);
  }
}
TEST(HmacSha256, Comparison) {
  EXPECT_TRUE(gm::constantTimeEqual("abc", "abc", 3));
  EXPECT_FALSE(gm::constantTimeEqual("abc", "xbc", 3));
  EXPECT_FALSE(gm::constantTimeEqual("abc", "abx", 3));
  EXPECT_TRUE(gm::constantTimeEqual(nullptr, nullptr, 0));
}
namespace {
std::string hexBytes(const uint8_t* bytes, size_t len) {
  static const char* abc = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < len; ++i) {
    out += abc[bytes[i] >> 4];
    out += abc[bytes[i] & 15];
  }
  return out;
}
std::string unhex(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); i += 2)
    out += static_cast<char>(std::stoul(text.substr(i, 2), nullptr, 16));
  return out;
}
}  // namespace
TEST(HmacSha256, Pbkdf2KnownSha256VectorsAndRfc7914) {
  struct Vector {
    const char* password;
    const char* salt;
    uint32_t iterations;
    const char* expected;
  };
  const Vector vectors[] = {
      {"password", "salt", 1,
       "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"},
      {"password", "salt", 2,
       "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"},
      {"password", "salt", 4096,
       "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"},
      {"passwd", "salt", 1,
       "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
       "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"},
      {"Password", "NaCl", 80000,
       "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
       "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d"}};
  for (const auto& v : vectors) {
    uint8_t out[64];
    const size_t len = strlen(v.expected) / 2;
    ASSERT_TRUE(gm::pbkdf2HmacSha256(v.password, strlen(v.password), v.salt,
                                     strlen(v.salt), v.iterations, out, len));
    EXPECT_EQ(hexBytes(out, len), v.expected);
  }
  uint8_t out[32];
  EXPECT_FALSE(gm::pbkdf2HmacSha256("p", 1, "s", 1, 0, out, 32));
}
TEST(HmacSha256, SharedGm2VectorsAndStreamingChunks) {
  std::ifstream file(std::string(GM_TEST_PORTAL_DIR) +
                     "/../tests/data/gm2-vectors.json");
  ASSERT_TRUE(file.good());
  // Not istreambuf_iterator: GCC 13 -O2 reports a false -Wnull-dereference.
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string json = buffer.str();
  const std::string password = gm2test::field(json.c_str(), "password"),
                    id = gm2test::field(json.c_str(), "id"),
                    nonce = gm2test::field(json.c_str(), "nonce");
  uint8_t root[32], mac[32], enc[32];
  const std::string salt = "gm2|" + id;
  ASSERT_TRUE(gm::pbkdf2HmacSha256(password.data(), password.size(),
                                   salt.data(), salt.size(), 10000, root, 32));
  gm::gm2DeriveKeys(password.c_str(), id.c_str(), mac, enc);
  EXPECT_EQ(hexBytes(root, 32), gm2test::field(json.c_str(), "K"));
  EXPECT_EQ(hexBytes(mac, 32), gm2test::field(json.c_str(), "Kmac"));
  EXPECT_EQ(hexBytes(enc, 32), gm2test::field(json.c_str(), "Kenc"));
  for (const char* direction : {"req", "res"}) {
    const size_t keyPos = json.find(std::string("\"") + direction + "\":");
    ASSERT_NE(keyPos, std::string::npos);
    const char* fields = json.c_str() + json.find('{', keyPos);
    const std::string client = gm2test::field(fields, "cid"),
                      mid = gm2test::field(fields, "mid"),
                      iv = gm2test::field(fields, "iv");
    const std::string plain = unhex(gm2test::field(fields, "plaintext_hex"));
    std::string encrypted = plain;
    // Deliberately split inside a stream block to check offset handling.
    for (size_t off = 0; off < encrypted.size();) {
      const size_t n = std::min<size_t>(13, encrypted.size() - off);
      gm::gm2StreamXor(enc, id.c_str(), nonce.c_str(), client.c_str(), 7,
                       mid.c_str(), direction, iv.c_str(), &encrypted[off], n,
                       off);
      off += n;
    }
    EXPECT_EQ(gm2test::base64(encrypted), gm2test::field(fields, "body"));
    EXPECT_EQ(gm2test::hmac(mac, gm2test::field(fields, "text")),
              gm2test::field(fields, "mac"));
    gm::gm2StreamXor(enc, id.c_str(), nonce.c_str(), client.c_str(), 7,
                     mid.c_str(), direction, iv.c_str(), &encrypted[0],
                     encrypted.size());
    EXPECT_EQ(encrypted, plain);
  }
}
