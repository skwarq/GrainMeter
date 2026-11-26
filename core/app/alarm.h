// SPDX-License-Identifier: MIT
#ifndef GM_ALARM_H
#define GM_ALARM_H
#include "app/config.h"
#include "data/alarm_state.h"
#include "data/history_log.h"
namespace gm {
void alarmEvaluate(AlarmState& a, const Config& cfg, const MeasureReport& m,
                   HistoryLog& history, uint32_t maxGapS);
bool alarmWindowDue(const AlarmState& a, const Config& cfg, uint32_t monoS);
const char* alarmReasonName(uint8_t reason);
}  // namespace gm
#endif
