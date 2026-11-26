// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

#include "util/json.h"

// Wake / awake timing for the energy budget, kept in RTC memory across deep
// sleep (reset on power loss). Shown as "power_stats" in /api/diag.
//   boot   - ROM + bootloader + image check before app_main (timer wakes)
//   awake  - app_main start .. deep sleep
//   wifi   - radio on for the station: connect start .. link dropped
void powerStatsBoot(bool fromDeepSleep);
void powerStatsWifiStart();
void powerStatsWifiConnected();
void powerStatsWifiEnd();
void powerStatsSntp(uint32_t ms);
// Called right before deep sleep; returns this wake's awake time in ms.
uint32_t powerStatsSleep(uint32_t sleepS);
uint32_t powerStatsLastBootMs();
uint32_t powerStatsLastWifiMs();
// -1 until at least one hour of elapsed operating time is available.
int32_t powerStatsMahPerDayCenti();
void powerStatsWrite(gm::JsonWriter& w);
