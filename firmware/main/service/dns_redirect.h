// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Captive-portal DNS: answers every A query with the SoftAP address so phones
// open the portal (with strict bounds checks on the received packet).
bool dnsRedirectStart(uint32_t ipv4NetworkOrder);
void dnsRedirectStop();
