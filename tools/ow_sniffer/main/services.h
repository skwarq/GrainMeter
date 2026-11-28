// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "esp_err.h"

namespace ow {
esp_err_t dnsStart(uint32_t ip);
esp_err_t webStart();
}  // namespace ow
