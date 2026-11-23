// SPDX-License-Identifier: MIT
#ifndef GM_CRC_H
#define GM_CRC_H

#include <stddef.h>
#include <stdint.h>

namespace gm {

// Dallas/Maxim CRC-8 (polynomial x^8 + x^5 + x^4 + 1, reflected 0x8C), as used
// by 1-Wire ROM codes and DS18B20 scratchpads. Running the CRC over data that
// already includes its CRC byte yields 0.
uint8_t crc8Maxim(const uint8_t* data, size_t len);

// CRC-32 (IEEE 802.3, reflected 0xEDB88320). Used to detect corruption of
// state kept in RTC memory or local files.
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);

}  // namespace gm

#endif
