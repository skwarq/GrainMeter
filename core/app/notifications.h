// SPDX-License-Identifier: MIT
#ifndef GM_NOTIFICATIONS_H
#define GM_NOTIFICATIONS_H
#include "app/platform.h"
#include "data/alarm_state.h"
namespace gm {
struct NotifyResult {
  bool enabled;
  bool ok;
  int status;
};
bool notificationsConfigured(const Config& cfg);
NotifyResult notifySend(const Config& cfg, HttpClient& http, unsigned channel,
                        const char* text);
void notifyText(const Config& cfg, const NotifyEvent& e, char* out, size_t cap);
}  // namespace gm
#endif
