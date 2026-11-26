// SPDX-License-Identifier: MIT
#include "service/session_hold.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_until[2] = {};
void holdAcquire(HoldOwner owner, uint32_t untilMonoS) {
  portENTER_CRITICAL(&s_mux);
  s_until[static_cast<unsigned>(owner)] = untilMonoS;
  portEXIT_CRITICAL(&s_mux);
}
void holdRelease(HoldOwner owner) { holdAcquire(owner, 0); }
bool holdActive(HoldOwner owner) {
  const uint32_t now = esp_timer_get_time() / 1000000;
  portENTER_CRITICAL(&s_mux);
  const uint32_t until = s_until[static_cast<unsigned>(owner)];
  const bool active = until != 0 && static_cast<int32_t>(until - now) > 0;
  portEXIT_CRITICAL(&s_mux);
  return active;
}
bool holdActive() {
  const uint32_t now = esp_timer_get_time() / 1000000;
  portENTER_CRITICAL(&s_mux);
  const bool active =
      (s_until[0] != 0 && static_cast<int32_t>(s_until[0] - now) > 0) ||
      (s_until[1] != 0 && static_cast<int32_t>(s_until[1] - now) > 0);
  portEXIT_CRITICAL(&s_mux);
  return active;
}
