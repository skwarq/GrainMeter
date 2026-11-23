// SPDX-License-Identifier: MIT
#pragma once

// Status LED (GPIO15 on FireBeetle 2 ESP32-C6) and service button (BOOT,
// GPIO9). The button is sampled only after the application started, so it
// never interferes with the ROM bootloader strapping.
void boardIoInit();
void ledSet(bool on);
void ledBlink(int periodMs);  // 0 = stop blinking (LED off)
bool serviceButtonPressed();  // debounced, active low
// Optional LP-GPIO service button (wakes from deep sleep); false if none.
bool serviceWakeButtonPressed();
// Blinks quickly and waits up to windowMs for the button; true if pressed.
bool waitForServiceButton(int windowMs);
