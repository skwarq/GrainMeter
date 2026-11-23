// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "esp_err.h"

// esp_http_server front end for the shared gm::Portal: embedded UI
// (gzip), /api/*, streaming OTA upload and captive-portal redirects.
esp_err_t webPortalStart();
void webPortalStop();
// Seconds since the last HTTP request (for the service-mode idle timeout).
uint32_t webPortalIdleSeconds();
