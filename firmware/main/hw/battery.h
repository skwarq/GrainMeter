// SPDX-License-Identifier: MIT
#pragma once

// Battery voltage on FireBeetle 2 ESP32-C6: VBAT -> 1M/1M divider -> GPIO0.
// Returns millivolts, or -1 when disabled or the ADC failed.
int batteryReadMv();
