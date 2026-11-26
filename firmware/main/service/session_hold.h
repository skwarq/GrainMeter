// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

enum class HoldOwner { Lan, Remote };
// Deadlines use esp_timer_get_time()/1000000 (this boot's monotonic clock).
void holdAcquire(HoldOwner owner, uint32_t untilMonoS);
void holdRelease(HoldOwner owner);
bool holdActive();
bool holdActive(HoldOwner owner);
