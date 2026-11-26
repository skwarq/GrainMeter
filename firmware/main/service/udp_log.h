// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

// Log forwarding over UDP (like WIFI_RS): every ESP_LOG line still goes to
// the USB console and, when enabled, also to host:port (empty host =
// broadcast 255.255.255.255) while the station link is up. Lines are
// prefixed with `prefix` (the SoftAP name, e.g. "GrainMeter-0079").
// level: 1 error, 2 warning, 3 info, 4 debug (applies to all outputs while
// forwarding is on; info otherwise). Receive with: nc -ulk <port>
void udpLogApply(bool enabled, uint32_t level, const char* host, uint16_t port,
                 const char* prefix);

// Lines dropped because the queue was full (diagnostics).
uint32_t udpLogDropped();
uint32_t udpLogSent();

// Second consumer of log lines (LAN console session): lines are copied into
// rb (non-blocking, dropped when full). nullptr stops it. Requires
// udpLogApply() to have run once (installs the log hook).
void logTapSet(RingbufHandle_t rb);
