// SPDX-License-Identifier: MIT
#include "app/notifications.h"

#include <stdio.h>
#include <string.h>

#include "app/grainmeter.h"
namespace gm {
bool notificationsConfigured(const Config& c) {
  return c.notifyNtfyUrl[0] || (c.notifyTgToken[0] && c.notifyTgChat[0]);
}
NotifyResult notifySend(const Config& c, HttpClient& http, unsigned channel,
                        const char* text) {
  NotifyResult result = {false, false, -1};
  char url[256], body[1024], resp[128] = "";
  JsonWriter w(body, sizeof(body));
  if (channel == 0) {
    if (!c.notifyNtfyUrl[0]) return result;
    const char* scheme = strstr(c.notifyNtfyUrl, "://");
    const char* path = scheme ? strchr(scheme + 3, '/') : nullptr;
    if (!path) return result;
    snprintf(url, sizeof(url), "%.*s", static_cast<int>(path - c.notifyNtfyUrl),
             c.notifyNtfyUrl);
    w.beginObject()
        .kv("topic", path + 1)
        .kv("title", c.deviceName)
        .kv("message", text)
        .kvu("priority", 4)
        .key("tags")
        .beginArray()
        .str("warning")
        .endArray()
        .endObject();
  } else {
    if (!c.notifyTgToken[0] || !c.notifyTgChat[0]) return result;
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage",
             c.notifyTgToken);
    w.beginObject().kv("chat_id", c.notifyTgChat).kv("text", text).endObject();
  }
  result.enabled = true;
  if (!w.ok()) return result;
  // Deliberately discard transport error text and response: neither is safe
  // to expose for Telegram requests whose URL contains a credential.
  const HttpResult r = http.postJson(url, body, strlen(body), kHttpTimeoutMs,
                                     resp, sizeof(resp));
  result.status = r.status;
  result.ok = r.transportOk && r.status >= 200 && r.status < 300;
  if (channel == 1) {
    // Telegram can return an API failure in an HTTP response.
    const char* ok = strstr(resp, "\"ok\"");
    if (ok) {
      ok += 4;
      while (*ok == ' ' || *ok == '\n' || *ok == '\r' || *ok == '\t') ++ok;
      if (*ok == ':') {
        ++ok;
        while (*ok == ' ' || *ok == '\n' || *ok == '\r' || *ok == '\t') ++ok;
      }
    }
    result.ok = result.ok && ok && strncmp(ok, "true", 4) == 0;
  }
  memset(url, 0, sizeof(url));
  return result;
}
void notifyText(const Config& c, const NotifyEvent& e, char* out, size_t cap) {
  if (e.type == NotifyFrostCharge) {
    snprintf(out, cap,
             "%s: akumulator ładuje się przy %.1f °C. Ogniwa LiPo nie wolno "
             "ładować poniżej 0 °C – odłącz panel albo przenieś ogniwo w "
             "cieplejsze miejsce.",
             c.deviceName, e.centi / 100.0);
  } else if (e.type == NotifyBattery) {
    snprintf(out, cap, "%s: niska bateria %d mV (%s).", c.deviceName, e.centi,
             e.reason == 2 ? "critical" : "economy");
  } else if (e.type == NotifyClear) {
    snprintf(out, cap, "%s: alarm ustąpił (punkt %u).", c.deviceName, e.point);
  } else {
    snprintf(out, cap,
             "%s: alarm %s, punkt %u, temperatura %.2f °C, wzrost %.2f °C.",
             c.deviceName, e.reason == AlarmTemp ? "temperatury" : "wzrostu",
             e.point, e.centi / 100.0, e.riseCenti / 100.0);
  }
}
void GrainMeter::sendNotifications() {
  AlarmState& a = st_->alarm;
  const uint32_t now = p_.clock->monoS();
  for (size_t i = 0; i < a.count;) {
    NotifyEvent& e = a.events[i];
    const uint8_t bit = static_cast<uint8_t>(1u << e.type);
    const uint32_t interval =
        e.type == NotifyBattery || e.type == NotifyFrostCharge
            ? 86400u
            : cfg_.alarmRepeatMin * 60u;
    if (!e.delivered && (a.deliveredTypes & bit) &&
        now - a.deliveredMonoS[e.type] < interval) {
      ++i;
      continue;
    }
    char text[320];
    notifyText(cfg_, e, text, sizeof(text));
    bool sent = false;
    for (unsigned ch = 0; ch < 2; ++ch) {
      const uint8_t mask = static_cast<uint8_t>(1u << ch);
      if (e.delivered & mask) continue;
      const NotifyResult r = notifySend(cfg_, *p_.http, ch, text);
      if (!r.enabled || r.ok) e.delivered |= mask;
      if (r.enabled && r.ok) sent = true;
    }
    if (sent) {
      a.deliveredTypes |= bit;
      a.deliveredMonoS[e.type] = now;
      a.lastNotifyMonoS = now;
    }
    if (e.delivered == 3) {
      memmove(a.events + i, a.events + i + 1,
              (a.count - i - 1) * sizeof(NotifyEvent));
      --a.count;
    } else
      ++i;
  }
  st_->seal();
}
void GrainMeter::notifyTest(JsonWriter& w) {
  char err[64], text[128];
  snprintf(text, sizeof(text), "%s: test powiadomienia.", cfg_.deviceName);
  const bool connected =
      p_.net->connect(cfg_, kWifiConnectTimeoutMs, err, sizeof(err));
  const bool ready = connected && (p_.net->syncTime(kTimeSyncTimeoutMs) ||
                                   p_.clock->wallValid());
  w.beginObject();
  for (unsigned ch = 0; ch < 2; ++ch) {
    NotifyResult r = {ch == 0 ? cfg_.notifyNtfyUrl[0] != 0
                              : cfg_.notifyTgToken[0] && cfg_.notifyTgChat[0],
                      false, -1};
    if (ready) r = notifySend(cfg_, *p_.http, ch, text);
    w.key(ch == 0 ? "ntfy" : "telegram")
        .beginObject()
        .kvb("enabled", r.enabled)
        .kvb("ok", r.ok)
        .kv("http_status", static_cast<int64_t>(r.status))
        .endObject();
  }
  w.endObject();
  p_.http->endSession();
  p_.net->disconnect();
}
}  // namespace gm
