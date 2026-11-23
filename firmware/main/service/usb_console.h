// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

// Service console on USB Serial/JTAG (works without any network). Lines
// are executed by the shared gm::Shell under the device lock.
void usbConsoleStart();
bool usbConnected();
// Seconds since the last command line.
uint32_t usbConsoleIdleSeconds();
