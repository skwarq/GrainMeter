// SPDX-License-Identifier: MIT
#ifndef GM_ALARM_STATE_H
#define GM_ALARM_STATE_H
#include <stdint.h>
namespace gm {
enum { kNotifySlots = 6, kNotifyTypes = 4 };
enum NotifyType : uint8_t {
  NotifyAlarm = 0,
  NotifyClear = 1,
  NotifyBattery = 2,
  NotifyFrostCharge = 3  // charging while the board is at/below ~0 °C
};
enum AlarmReason : uint8_t { AlarmNone = 0, AlarmTemp = 1, AlarmRise = 2 };
struct NotifyEvent {
  uint32_t epoch;
  uint32_t monoS;
  int16_t centi;
  int16_t riseCenti;
  uint8_t type;
  uint8_t reason;
  uint8_t point;
  uint8_t delivered;  // bit 0 ntfy, bit 1 Telegram
};
struct AlarmState {
  uint32_t startEpoch;
  uint32_t startMonoS;
  uint32_t lastWindowMonoS;
  uint32_t lastNotifyMonoS;
  uint32_t deliveredMonoS[kNotifyTypes];
  uint32_t dropped;
  uint16_t
      samplesSentSinceRegular;  // alarm uploads count toward the ordinary batch
  int16_t centi;
  int16_t riseCenti;
  uint8_t active;
  uint8_t reason;
  uint8_t point;
  uint8_t tempMask;  // independent hysteresis latch per mapped point
  uint8_t pendingWindow;
  uint8_t hasWindow;
  uint8_t deliveredTypes;
  uint8_t powerMode;
  uint8_t count;
  NotifyEvent events[kNotifySlots];
};
void alarmEnqueue(AlarmState& a, const NotifyEvent& event);
}  // namespace gm
#endif
