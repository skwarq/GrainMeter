// SPDX-License-Identifier: MIT
#ifndef GM_TEST_FAKE_IMAGE_H
#define GM_TEST_FAKE_IMAGE_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Minimal ESP-IDF application image header + app descriptor, enough for the
// shared OTA validation (the PC target does not check the SHA-256 that
// esp_ota_end verifies on the device).
inline std::vector<uint8_t> fakeImage(size_t size, uint16_t chip = 0x000D,
                                      const char* project = "grainmeter",
                                      const char* version = "9.9.9-test",
                                      uint8_t magic = 0xE9) {
  std::vector<uint8_t> img(size, 0x5A);
  if (size < 300) return img;
  std::memset(img.data(), 0, 300);
  img[0] = magic;
  img[1] = 3;
  img[12] = static_cast<uint8_t>(chip & 0xFF);
  img[13] = static_cast<uint8_t>(chip >> 8);
  const uint32_t descMagic = 0xABCD5432u;
  std::memcpy(&img[32], &descMagic, 4);  // little endian hosts only
  std::strncpy(reinterpret_cast<char*>(&img[48]), version, 31);
  std::strncpy(reinterpret_cast<char*>(&img[80]), project, 31);
  std::strncpy(reinterpret_cast<char*>(&img[144]), "v6.1.1", 31);
  return img;
}

inline std::string multipartBody(const std::string& boundary,
                                 const std::vector<uint8_t>& file,
                                 const std::string& fileName) {
  std::string b = "--" + boundary + "\r\n";
  b += "Content-Disposition: form-data; name=\"note\"\r\n\r\nhello\r\n";
  b += "--" + boundary + "\r\n";
  b += "Content-Disposition: form-data; name=\"firmware\"; filename=\"" +
       fileName + "\"\r\n";
  b += "Content-Type: application/octet-stream\r\n\r\n";
  b.append(reinterpret_cast<const char*>(file.data()), file.size());
  b += "\r\n--" + boundary + "--\r\n";
  return b;
}

#endif
