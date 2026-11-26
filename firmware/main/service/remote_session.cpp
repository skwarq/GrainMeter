// SPDX-License-Identifier: MIT
#include "service/remote_session.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "app/portal_tunnel.h"
#include "device.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "service/session_hold.h"
#include "service/usb_console.h"
#include "util/crc.h"
#include "util/json_read.h"

namespace {
constexpr const char* kTag = "remote";
constexpr const char* kOffline = "{\"v\":2,\"state\":\"offline\"}";
constexpr uint32_t kMagic = 0x524D5331;
uint32_t nowS() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000000);
}
struct Resume {
  uint32_t magic, remaining, untilEpoch;
  // part: partition selected for the next boot when the restart was
  // requested; booting from another one after restart means a rollback.
  char mid[17], fw[33], target[33], part[17];
  uint32_t crc;
};
RTC_NOINIT_ATTR Resume s_resume;
// Control and diagnostics are protected by DeviceLock. Only MQTT event flags
// cross tasks without that lock. No callback ever waits for DeviceLock.
uint32_t s_pendingAt = 0;
uint32_t s_pending = 0, s_since = 0, s_until = 0, s_lastValid = 0;
uint32_t s_requests = 0;
portMUX_TYPE s_energyMux = portMUX_INITIALIZER_UNLOCKED;
esp_timer_handle_t s_energyTimer = nullptr;
bool s_energyActive = false, s_energyUsb = false;
uint32_t s_energyAt = 0, s_unbilled = 0;
std::atomic<uint32_t> s_rejected{0};
char s_targetPart[17] = "";
char s_id[25] = "", s_error[64] = "", s_opMid[17] = "", s_oldFw[33] = "",
     s_targetFw[33] = "";
bool s_stop = false, s_stopping = false;
wifi_ps_type_t s_previousPs = WIFI_PS_MIN_MODEM;
std::atomic<bool> s_operation{false}, s_restarting{false};
uint32_t s_resumeSeconds = 0;
struct Request {
  uint32_t generation;
  size_t len;
  bool retained;
  char mid[17], payload[4097];
};
struct Session {
  explicit Session() : tunnel(*device().portal, *device().app) {}
  gm::PortalTunnel tunnel;
  gm::Config cfg;
  esp_mqtt_client_handle_t client = nullptr;
  QueueHandle_t queue = nullptr, acks = nullptr;
  SemaphoreHandle_t done = nullptr;
  std::atomic<bool> stop{false}, online{false};
  std::atomic<uint32_t> generation{0};
  std::atomic<uint32_t> logUsers{0};
  std::atomic<int> error{0};
  Request incoming{}, work{};
  char response[gm::PortalTunnel::kMaxResponse + 256];
  char statusTopic[48], reqTopic[48], clientId[40], nonce[33];
  bool statusDirty = false;  // protected by DeviceLock
};
Session* s_session = nullptr;

// The timer task samples USB independently of DeviceLock, including during a
// blocking portal update. Only the main/worker task calls into GrainMeter.
void samplePower(void*) {
  const uint32_t now = nowS();
  const bool usb = usbConnected();
  portENTER_CRITICAL(&s_energyMux);
  if (s_energyActive && now >= s_energyAt) {
    if (!s_energyUsb) s_unbilled += now - s_energyAt;
    s_energyAt = now;
    s_energyUsb = usb;
  }
  portEXIT_CRITICAL(&s_energyMux);
}
void account(bool flush) {
  samplePower(nullptr);
  uint32_t used = 0;
  portENTER_CRITICAL(&s_energyMux);
  if (flush || s_unbilled >= 60) {
    used = s_unbilled;
    s_unbilled = 0;
  }
  portEXIT_CRITICAL(&s_energyMux);
  if (used) device().app->remoteSessionUsed(used);
}
uint32_t unbilled() {
  portENTER_CRITICAL(&s_energyMux);
  const uint32_t seconds = s_unbilled;
  portEXIT_CRITICAL(&s_energyMux);
  return seconds;
}
bool startEnergy() {
  const bool usb = usbConnected();
  portENTER_CRITICAL(&s_energyMux);
  s_unbilled = 0;
  s_energyAt = nowS();
  s_energyUsb = usb;
  s_energyActive = true;
  portEXIT_CRITICAL(&s_energyMux);
  esp_timer_create_args_t args{};
  args.callback = samplePower;
  args.name = "remote_power";
  return esp_timer_create(&args, &s_energyTimer) == ESP_OK &&
         esp_timer_start_periodic(s_energyTimer, 1000000) == ESP_OK;
}
void stopEnergy() {
  samplePower(nullptr);
  portENTER_CRITICAL(&s_energyMux);
  s_energyActive = false;
  portEXIT_CRITICAL(&s_energyMux);
  if (s_energyTimer) {
    esp_timer_stop(s_energyTimer);
    esp_timer_delete(s_energyTimer);
    s_energyTimer = nullptr;
  }
  // Any already dispatched callback accesses only the globals above, never
  // the Session object, and sees s_energyActive=false under the same mux.
}
void saveResume(const char* mid, const char* target) {
  account(true);
  s_resume = {};
  const uint32_t now = nowS();
  if (s_until <= now) return;
  s_resume.magic = kMagic;
  s_resume.remaining = s_until - now;
  s_resume.untilEpoch = device().clock.wallValid()
                            ? device().clock.epoch() + s_resume.remaining
                            : 0;
  snprintf(s_resume.mid, sizeof(s_resume.mid), "%s", mid);
  snprintf(s_resume.fw, sizeof(s_resume.fw), "%s",
           esp_app_get_description()->version);
  snprintf(s_resume.target, sizeof(s_resume.target), "%s", target);
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  snprintf(s_resume.part, sizeof(s_resume.part), "%s", boot ? boot->label : "");
  s_resume.crc = gm::crc32(&s_resume, offsetof(Resume, crc));
}
// Unsigned error reply sent straight from the MQTT task, for requests that
// never reach the tunnel (oversized/fragmented payload, full queue). Signing
// needs the device lock, which this task must not wait for; a forged copy of
// such a reply can only make the browser retry, which a broker-side attacker
// could cause anyway by dropping the request.
uint64_t peekSeq(const char* data, int len) {
  static const char kKey[] = "\"seq\":";
  const int keyLen = sizeof(kKey) - 1;
  for (int i = 0; i + keyLen < len; ++i) {
    if (memcmp(data + i, kKey, keyLen) != 0) continue;
    uint64_t seq = 0;
    int j = i + keyLen, digits = 0;
    while (j < len && data[j] >= '0' && data[j] <= '9' && digits < 19) {
      seq = seq * 10 + static_cast<uint64_t>(data[j++] - '0');
      ++digits;
    }
    return digits ? seq : 0;
  }
  return 0;
}
void replyUnsigned(Session* s, const char* mid, const char* data, int len,
                   int status, const char* code) {
  char topic[64], body[160];
  snprintf(topic, sizeof(topic), "gm/%s/res/%s", s_id, mid);
  snprintf(body, sizeof(body),
           "{\"v\":2,\"seq\":%llu,\"status\":%d,\"ctype\":"
           "\"application/json\",\"body\":\"{\\\"error\\\":\\\"%s\\\"}\","
           "\"mac\":\"\",\"restart\":false}",
           static_cast<unsigned long long>(peekSeq(data, len)), status, code);
  esp_mqtt_client_publish(s->client, topic, body, 0, 0, 0);
}
void event(void* arg, esp_event_base_t, int32_t eventId, void* data) {
  auto* s = static_cast<Session*>(arg);
  auto* e = static_cast<esp_mqtt_event_t*>(data);
  if (eventId == MQTT_EVENT_CONNECTED) {
    s->generation.fetch_add(1);
    s->online = true;
  } else if (eventId == MQTT_EVENT_DISCONNECTED) {
    s->online = false;
  } else if (eventId == MQTT_EVENT_ERROR) {
    // No broker credentials or payloads in diagnostics/logs.
    s->error = 1;
    ESP_LOGW(kTag, "MQTT connection error");
  } else if (eventId == MQTT_EVENT_PUBLISHED) {
    xQueueSend(s->acks, &e->msg_id, 0);
  } else if (eventId == MQTT_EVENT_DATA && !s->stop) {
    // Later fragments of a large message carry no topic: the first one
    // already answered it.
    if (e->current_data_offset != 0) return;
    const bool oversized = e->total_data_len != e->data_len ||
                           e->data_len < 0 || e->data_len > 4096;
    char prefix[48];
    const int prefixLen = snprintf(prefix, sizeof(prefix), "gm/%s/req/", s_id);
    const int midLen = e->topic_len - prefixLen;
    if (midLen < 1 || midLen > 16 || e->topic == nullptr ||
        memcmp(e->topic, prefix, prefixLen) != 0) {
      ++s_rejected;
      return;
    }
    Request& r = s->incoming;  // used only by the MQTT task
    for (int i = 0; i < midLen; ++i) {
      const char c = e->topic[prefixLen + i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) {
        ++s_rejected;
        return;
      }
      r.mid[i] = c;
    }
    r.mid[midLen] = 0;
    if (oversized) {
      ++s_rejected;
      ESP_LOGW(kTag, "oversized MQTT request (%d B)", e->total_data_len);
      if (!e->retain)
        replyUnsigned(s, r.mid, e->data, e->data_len > 0 ? e->data_len : 0, 400,
                      "bad_request");
      return;
    }
    r.len = static_cast<size_t>(e->data_len);
    if (r.len) memcpy(r.payload, e->data, r.len);
    r.payload[r.len] = 0;
    r.retained = e->retain;
    r.generation = s->generation.load();
    // Drop on saturation: never block MQTT callbacks behind a portal update.
    // The browser times out/retries; the queue and response memory stay
    // bounded.
    if (xQueueSend(s->queue, &r, 0) != pdTRUE) {
      ++s_rejected;
      ESP_LOGW(kTag, "MQTT request while busy: 503");
      if (!r.retained)
        replyUnsigned(s, r.mid, r.payload, static_cast<int>(r.len), 503,
                      "busy");
    }
  }
}
void logKdfTime(int64_t started) {
  const int64_t elapsed = (esp_timer_get_time() - started) / 1000;
  ESP_LOGI(kTag, "gm2 PBKDF2: %u iterations, %lld ms",
           static_cast<unsigned>(gm::PortalTunnel::kdfIterations()),
           static_cast<long long>(elapsed));
  if (elapsed > 2000)
    ESP_LOGW(kTag,
             "gm2 PBKDF2 exceeds 2 s; tune iteration count on this board");
}
void refreshKeys(Session* s) {
  const int64_t started = esp_timer_get_time();
  if (s->tunnel.refreshKeys()) {
    logKdfTime(started);
    s->statusDirty = true;
  }
}
void publishStatus(Session* s, bool newSession) {
  char status[1024], name[32], mac[65];
  {
    DeviceLock lock;
    if (newSession) {
      for (unsigned i = 0; i < 4; ++i) {
        const uint32_t random = esp_random();
        snprintf(s->nonce + i * 8, 9, "%08lx",
                 static_cast<unsigned long>(random));
      }
      const int64_t started = esp_timer_get_time();
      s->tunnel.beginSession(s->nonce, s_id);
      logKdfTime(started);
    } else {
      refreshKeys(s);
    }
    s->statusDirty = false;
    account(true);
    device().sys.copyDeviceName(name, sizeof(name));
    gm::JsonWriter w(status, sizeof(status));
    const uint32_t remaining = s_until > nowS() ? s_until - nowS() : 0;
    const uint32_t until =
        device().clock.wallValid() ? device().clock.epoch() + remaining : 0;
    const uint32_t budget = device().app->remoteBudgetLeftS();
    const char* opResult = "";
    w.beginObject()
        .kvu("v", 2)
        .kv("state", "online")
        .kv("nonce", s->nonce)
        .kv("fw", esp_app_get_description()->version)
        .kv("name", name)
        .kvu("until", until)
        .kvu("remaining_s", remaining)
        .kvu("budget_left_s", budget)
        .kvu("kdf_iter", gm::PortalTunnel::kdfIterations())
        .key("op");
    if (s_opMid[0]) {
      const char* running = esp_app_get_description()->version;
      bool succeeded = strcmp(s_oldFw, running) != 0;
      // When a target was known before restart, this also detects rollback.
      if (s_targetFw[0]) succeeded = strcmp(s_targetFw, running) == 0;
      // The partition decides: a plain restart boots the same slot (ok even
      // with an unchanged version); a rollback boots the other one (failed
      // even when both images carry the same version).
      if (s_targetPart[0]) {
        const esp_partition_t* now = esp_ota_get_running_partition();
        succeeded = now && strcmp(now->label, s_targetPart) == 0 &&
                    (!s_targetFw[0] || strcmp(s_targetFw, running) == 0);
      }
      opResult = succeeded ? "ok" : "failed";
      w.beginObject()
          .kv("mid", s_opMid)
          .kv("fw", esp_app_get_description()->version)
          .kv("result", opResult)
          .endObject();
    } else {
      w.null();
    }
    s->tunnel.statusMac(esp_app_get_description()->version, name, until,
                        remaining, budget, s_opMid,
                        s_opMid[0] ? esp_app_get_description()->version : "",
                        opResult, mac);
    w.kv("mac", mac).endObject();
    if (!w.ok()) {
      s->statusDirty = true;
      snprintf(s_error, sizeof(s_error), "MQTT status exceeds buffer");
      ESP_LOGE(kTag, "%s", s_error);
      return;
    }
  }
  if (newSession) esp_mqtt_client_subscribe(s->client, s->reqTopic, 1);
  if (esp_mqtt_client_publish(s->client, s->statusTopic, status, 0, 1, 1) < 0) {
    DeviceLock lock;
    s->statusDirty = true;
    snprintf(s_error, sizeof(s_error), "MQTT status publish failed");
    ESP_LOGW(kTag, "%s", s_error);
  }
}
void worker(void* arg) {
  auto* s = static_cast<Session*>(arg);
  uint32_t generation = 0;
  while (!s->stop) {
    const uint32_t current = s->generation.load();
    if (s->online && current != generation) {
      generation = current;
      publishStatus(s, true);
    } else if (s->online) {
      bool dirty;
      {
        DeviceLock lock;
        refreshKeys(s);
        dirty = s->statusDirty && !s_stop;
      }
      if (dirty) publishStatus(s, false);
    }
    if (xQueueReceive(s->queue, &s->work, pdMS_TO_TICKS(100)) != pdTRUE)
      continue;
    Request& r = s->work;
    if (!s->online || r.generation != generation) {
      ++s_rejected;
      continue;
    }
    gm::TunnelOut out{};
    bool reply;
    bool exit = false;
    {
      DeviceLock lock;
      if (s->stop || s_restarting) break;
      if (s_stop || nowS() >= s_until ||
          nowS() - s_lastValid >= device().app->config().remoteIdleMin * 60) {
        ++s_rejected;
        continue;
      }
      s_operation = true;
      refreshKeys(s);
      reply = s->tunnel.handle(r.mid, r.payload, r.len, r.retained,
                               device().clock.monoS(), &out);
      if (reply) {
        memcpy(s->response, out.payload, out.len);
        if (out.status != 400 && out.status != 401 && out.status != 429) {
          ++s_requests;
        } else {
          ++s_rejected;
        }
        char path[96], method[8];
        exit = out.status >= 200 && out.status < 300 &&
               gm::jsonGetString(r.payload, "path", path, sizeof(path)) &&
               gm::jsonGetString(r.payload, "method", method, sizeof(method)) &&
               strcmp(path, "/api/service/exit") == 0 &&
               strcmp(method, "POST") == 0;
      } else {
        ++s_rejected;
      }
    }
    if (!reply) {
      s_operation = false;
      continue;
    }
    char topic[64];
    snprintf(topic, sizeof(topic), "gm/%s/res/%s", s_id, r.mid);
    if (out.restart || exit) xQueueReset(s->acks);
    const int msgId = esp_mqtt_client_publish(s->client, topic, s->response,
                                              static_cast<int>(out.len), 1, 0);
    if (out.restart || exit) {
      const TickType_t start = xTaskGetTickCount();
      int ack = -1;
      while (msgId >= 0 && xTaskGetTickCount() - start < pdMS_TO_TICKS(2000)) {
        if (xQueueReceive(s->acks, &ack, pdMS_TO_TICKS(20)) == pdTRUE &&
            ack == msgId)
          break;
      }
      if (msgId < 0 || ack != msgId)
        ESP_LOGW(kTag, "MQTT response PUBACK timeout/failure");
    }
    if (out.restart) {
      {
        DeviceLock lock;
        const char* target = "";
        if (device().app->lastRemoteUpdateResult() ==
                gm::RemoteUpdateResult::Updated &&
            strcmp(device().app->lastRemoteUpdateVersion(),
                   esp_app_get_description()->version) != 0) {
          target = device().app->lastRemoteUpdateVersion();
        }
        // A finished chunked upload (/api/ota/finish) names its own target,
        // so the status after boot reports ok/failed (rollback) even when the
        // same version is reinstalled.
        const gm::OtaSession& ota = device().app->ota();
        if (ota.state() == gm::OtaSession::State::Done && ota.imageValid() &&
            ota.image().version[0])
          target = ota.image().version;
        saveResume(r.mid, target);
        device().sys.restart = true;
        s_operation = false;
      }
      // Do not process another portal operation before main performs restart.
      while (!s->stop) vTaskDelay(pdMS_TO_TICKS(50));
    }
    {
      DeviceLock lock;
      if (exit) s_stop = true;
      s_operation = false;
    }
  }
  xSemaphoreGive(s->done);
  vTaskDelete(nullptr);
}
void destroy(Session* s, bool taskStarted) {
  while (s->logUsers.load() != 0) vTaskDelay(pdMS_TO_TICKS(10));
  if (taskStarted) {
    s->stop = true;
    xSemaphoreTake(s->done, portMAX_DELAY);
  }
  if (s->client) {
    if (s->online) {
      xQueueReset(s->acks);
      const int id =
          esp_mqtt_client_publish(s->client, s->statusTopic, kOffline, 0, 1, 1);
      const TickType_t started = xTaskGetTickCount();
      int ack;
      while (id >= 0 && xTaskGetTickCount() - started < pdMS_TO_TICKS(2000)) {
        if (xQueueReceive(s->acks, &ack, pdMS_TO_TICKS(20)) == pdTRUE &&
            ack == id)
          break;
      }
    }
    esp_mqtt_client_disconnect(s->client);
    esp_mqtt_client_stop(s->client);
    esp_mqtt_client_destroy(s->client);
  }
  if (s->queue) vQueueDelete(s->queue);
  if (s->acks) vQueueDelete(s->acks);
  if (s->done) vSemaphoreDelete(s->done);
  delete s;
}
// Chunked OTA over the tunnel: a ~1.35 MB image takes 2-3 minutes. Make
// sure the session outlives it (extend to 15 min within the daily budget) or
// refuse before anything is erased. Runs under DeviceLock (portal request).
bool canBeginOta(void*, int* status, const char** error) {
  if (!s_session || s_stop) return true;  // local upload, no remote session
  const uint32_t now = nowS();
  uint32_t remaining = s_until > now ? s_until - now : 0;
  if (remaining < 15 * 60) {
    uint32_t budget =
        usbConnected() ? 15 * 60 : device().app->remoteBudgetLeftS();
    const uint32_t billed = unbilled();
    budget = budget > billed ? budget - billed : 0;
    const uint32_t target = budget < 15 * 60 ? budget : 15 * 60;
    if (target > remaining) {
      s_until = now + target;
      remaining = target;
    }
  }
  if (remaining < 8 * 60) {
    *status = 409;
    *error = "session_short";
    return false;
  }
  return true;
}
bool request(uint32_t minutes) {
  if (s_stopping) return false;
  if (s_session) return true;
  const auto& cfg = device().app->config();
  if (!cfg.remoteUrl[0]) return false;
  if (!cfg.servicePassword[0]) {
    ESP_LOGW(kTag, "remote disabled: service password not set");
    snprintf(s_error, sizeof(s_error), "service password not set");
    return false;
  }
  if (gm::powerModeFor(device().sys.batteryMv(), cfg) ==
      gm::PowerMode::Critical) {
    ESP_LOGW(kTag, "remote disabled: critical battery");
    snprintf(s_error, sizeof(s_error), "critical battery");
    return false;
  }
  if (!cfg.remoteBudgetMin) return false;
  device().app->otaUpload().setCanBegin(canBeginOta, nullptr);
  uint32_t seconds = minutes * 60;
  if (!usbConnected()) {
    const uint32_t left = device().app->remoteBudgetLeftS();
    if (seconds > left) seconds = left;
  }
  if (!seconds) return false;
  s_stop = false;
  s_pending = seconds;
  s_pendingAt = nowS();
  holdAcquire(HoldOwner::Remote, nowS() + seconds);
  return true;
}

class RemotePort : public gm::RemoteSessionPort {
 public:
  bool active() override { return s_session && !s_stop && !s_stopping; }
  void validRequest() override {
    if (active()) s_lastValid = nowS();
  }
  gm::RemoteSessionInfo info() override {
    if (s_session) account(true);
    gm::RemoteSessionInfo out;
    const uint32_t now = nowS();
    out.remainingS = active() && s_until > now ? s_until - now : 0;
    out.budgetLeftS = device().app->remoteBudgetLeftS();
    return out;
  }
  gm::RemoteExtendResult extend(uint32_t minutes,
                                gm::RemoteSessionInfo* out) override {
    if (!active()) return gm::RemoteExtendResult::NoSession;
    account(true);
    const uint32_t now = nowS();
    out->budgetLeftS = device().app->remoteBudgetLeftS();
    if (!out->budgetLeftS) return gm::RemoteExtendResult::Budget;
    out->remainingS = gm::remoteExtendedRemaining(
        s_until > now ? s_until - now : 0, minutes, out->budgetLeftS);
    s_until = now + out->remainingS;
    s_lastValid = now;
    holdAcquire(HoldOwner::Remote, s_until);
    s_session->statusDirty = true;
    return gm::RemoteExtendResult::Ok;
  }
};
}  // namespace

gm::RemoteSessionPort& remoteSessionPort() {
  static RemotePort port;
  return port;
}

bool remoteLogOnline() {
  DeviceLock lock;
  return s_session && !s_stop && !s_stopping && !s_session->stop &&
         s_session->online && !s_restarting;
}

bool remoteLogPublish(const char* text, size_t len) {
  Session* s;
  {
    DeviceLock lock;
    s = s_session;
    if (!s || s_stop || s_stopping || s->stop || !s->online || s_restarting)
      return false;
    ++s->logUsers;
  }
  char topic[48];
  snprintf(topic, sizeof(topic), "gm/%s/log",
           s->cfg.remoteId[0] ? s->cfg.remoteId : s_id);
  const int result = esp_mqtt_client_publish(s->client, topic, text,
                                             static_cast<int>(len), 0, 0);
  --s->logUsers;
  return result >= 0;
}

void remoteAfterUpload() {
  uint32_t minutes;
  if (device().app->takeRemoteRequest(&minutes)) request(minutes);
}
void remoteServiceStart() {
  DeviceLock lock;
  if (!s_session && !s_pending) request(60);
}
void remoteStop() {
  s_stop = true;  // caller holds DeviceLock
  s_pending = 0;
}
void remotePoll() {
  bool finish = false;
  {
    DeviceLock lock;
    if (!s_session && s_stop) {
      s_stop = false;
      holdRelease(HoldOwner::Remote);
    }
    device().app->otaUpload().tick();  // abandoned chunked upload -> abort
    if (s_session) {
      account(false);
      if (s_session->error.exchange(0))
        snprintf(s_error, sizeof(s_error), "MQTT connection error");
      const bool exhausted =
          !usbConnected() && device().app->remoteBudgetLeftS() <= unbilled();
      finish =
          s_stop || nowS() >= s_until ||
          nowS() - s_lastValid >= device().app->config().remoteIdleMin * 60 ||
          exhausted ||
          gm::powerModeFor(device().sys.batteryMv(), device().app->config()) ==
              gm::PowerMode::Critical ||
          device().app->config().remoteBudgetMin == 0 ||
          !device().app->config().remoteUrl[0] ||
          !device().app->config().servicePassword[0];
      finish = finish && !s_operation && !device().sys.restart;
      // A session ending mid-upload abandons it: the boot slot stays as is.
      if (finish && device().app->otaUpload().active())
        device().app->otaUpload().abort();
      // Keep the board awake until the worker and MQTT callbacks have drained,
      // even if the requested session deadline has already passed.
      if (!holdActive(HoldOwner::Remote))
        holdAcquire(HoldOwner::Remote, nowS() + 5);
      if (finish) account(true);
      // An already running session keeps its original deadline.
      s_pending = 0;
    }
  }
  if (finish) {
    Session* old;
    {
      DeviceLock lock;
      old = s_session;
      old->stop = true;
      s_session = nullptr;
      s_stopping = true;
    }
    destroy(old, true);
    stopEnergy();
    {
      DeviceLock lock;
      account(true);
      s_stop = false;
      s_stopping = false;
      s_opMid[0] = s_targetFw[0] = s_targetPart[0] = 0;
      holdRelease(HoldOwner::Remote);
    }
    if (!holdActive(HoldOwner::Lan)) esp_wifi_set_ps(s_previousPs);
    return;
  }
  gm::Config cfg;
  {
    DeviceLock lock;
    if (s_session || !s_pending) return;
    cfg = device().app->config();
    if (!cfg.remoteUrl[0] || !cfg.servicePassword[0] || !cfg.remoteBudgetMin ||
        gm::powerModeFor(device().sys.batteryMv(), cfg) ==
            gm::PowerMode::Critical) {
      s_pending = 0;
      holdRelease(HoldOwner::Remote);
      return;
    }
    s_since = nowS();
    if (s_since - s_pendingAt >= s_pending) {
      s_pending = 0;
      holdRelease(HoldOwner::Remote);
      return;
    }
    s_until = s_pendingAt + s_pending;
    s_pending = 0;
    s_lastValid = s_since;
    s_requests = 0;
    s_rejected = 0;
    s_error[0] = 0;
    if (cfg.remoteId[0])
      snprintf(s_id, sizeof(s_id), "%s", cfg.remoteId);
    else {
      uint8_t mac[6]{};
      esp_read_mac(mac, ESP_MAC_WIFI_STA);
      snprintf(s_id, sizeof(s_id), "gm-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
  }
  const bool energyOk = startEnergy();
  if (energyOk) {
    DeviceLock lock;
    if (!device().clock.wallValid()) {
      device().app->synchronizeTime();
    }
  }
  auto* s = energyOk ? new (std::nothrow) Session : nullptr;
  bool started = false;
  if (s) {
    s->cfg = cfg;
    s->queue = xQueueCreate(1, sizeof(Request));
    s->acks = xQueueCreate(8, sizeof(int));
    s->done = xSemaphoreCreateBinary();
    snprintf(s->statusTopic, sizeof(s->statusTopic), "gm/%s/status", s_id);
    snprintf(s->reqTopic, sizeof(s->reqTopic), "gm/%s/req/+", s_id);
    snprintf(s->clientId, sizeof(s->clientId), "gm-%s-dev", s_id);
    esp_mqtt_client_config_t mqtt{};
    mqtt.broker.address.uri = s->cfg.remoteUrl;
    mqtt.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt.credentials.username = s->cfg.remoteUser;
    mqtt.credentials.authentication.password = s->cfg.remotePass;
    mqtt.credentials.client_id = s->clientId;
    mqtt.session.keepalive = 30;
    mqtt.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    mqtt.session.last_will.topic = s->statusTopic;
    mqtt.session.last_will.msg = kOffline;
    mqtt.session.last_will.qos = 1;
    mqtt.session.last_will.retain = 1;
    mqtt.buffer.size = 4608;
    mqtt.buffer.out_size = 24 * 1024;
    mqtt.network.timeout_ms = 3000;
    mqtt.outbox.limit = 48 * 1024;
    if (s->queue && s->acks && s->done) s->client = esp_mqtt_client_init(&mqtt);
    if (s->client &&
        esp_mqtt_client_register_event(s->client, MQTT_EVENT_ANY, event, s) ==
            ESP_OK &&
        xTaskCreate(worker, "remote", 12288, s, 3, nullptr) == pdPASS) {
      started = true;
      if (esp_mqtt_client_start(s->client) == ESP_OK) {
        DeviceLock lock;
        s_session = s;
      }
    }
  }
  memset(&cfg, 0, sizeof(cfg));
  if (!s_session) {
    if (s) destroy(s, started);
    stopEnergy();
    DeviceLock lock;
    account(true);
    snprintf(s_error, sizeof(s_error), "MQTT initialization failed");
    holdRelease(HoldOwner::Remote);
  } else {
    device().wifi.setKeepStation(true);
    esp_wifi_get_ps(&s_previousPs);
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
}
void remoteResume() {
  DeviceLock lock;
  const bool valid =
      // A crash of a new image before it is confirmed (panic, watchdog) makes
      // the bootloader roll back: resume too, so the browser gets "failed".
      (esp_reset_reason() == ESP_RST_SW ||
       esp_reset_reason() == ESP_RST_PANIC ||
       esp_reset_reason() == ESP_RST_INT_WDT ||
       esp_reset_reason() == ESP_RST_TASK_WDT ||
       esp_reset_reason() == ESP_RST_WDT) &&
      s_resume.magic == kMagic &&
      s_resume.crc == gm::crc32(&s_resume, offsetof(Resume, crc)) &&
      s_resume.remaining > 0 && s_resume.remaining <= 3600 &&
      memchr(s_resume.mid, 0, sizeof(s_resume.mid)) &&
      memchr(s_resume.fw, 0, sizeof(s_resume.fw)) &&
      memchr(s_resume.target, 0, sizeof(s_resume.target)) &&
      memchr(s_resume.part, 0, sizeof(s_resume.part));
  if (valid) {
    s_resumeSeconds = s_resume.remaining;
    if (s_resume.untilEpoch && device().clock.wallValid()) {
      const uint32_t epoch = device().clock.epoch();
      s_resumeSeconds =
          s_resume.untilEpoch > epoch ? s_resume.untilEpoch - epoch : 0;
      if (s_resumeSeconds > s_resume.remaining)
        s_resumeSeconds = s_resume.remaining;
    }
    snprintf(s_opMid, sizeof(s_opMid), "%s", s_resume.mid);
    snprintf(s_oldFw, sizeof(s_oldFw), "%s", s_resume.fw);
    snprintf(s_targetFw, sizeof(s_targetFw), "%s", s_resume.target);
    snprintf(s_targetPart, sizeof(s_targetPart), "%s", s_resume.part);
    if (s_resumeSeconds && request((s_resumeSeconds + 59) / 60)) {
      if (s_pending > s_resumeSeconds) s_pending = s_resumeSeconds;
      holdAcquire(HoldOwner::Remote, nowS() + s_pending);
    }
  }
  s_resume.magic = 0;
}
void remoteBeforeRestart() {
  s_restarting = true;
  for (;;) {
    {
      DeviceLock lock;
      // Recheck under the same lock used to start a portal operation: a
      // worker may have passed its restart check just before we set the flag.
      if (!s_operation) {
        if (s_session) {
          char mid[17] = "", target[33] = "";
          if (s_resume.magic == kMagic) {
            snprintf(mid, sizeof(mid), "%s", s_resume.mid);
            snprintf(target, sizeof(target), "%s", s_resume.target);
          } else if (device().app->lastRemoteUpdateResult() ==
                         gm::RemoteUpdateResult::Updated &&
                     strcmp(device().app->lastRemoteUpdateVersion(),
                            esp_app_get_description()->version) != 0) {
            snprintf(target, sizeof(target), "%s",
                     device().app->lastRemoteUpdateVersion());
          }
          saveResume(mid, target);
        }
        return;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
void remoteWriteJson(gm::JsonWriter& w) {
  // Resolve the topic id even before the first connection, for provisioning.
  char id[25];
  snprintf(id, sizeof(id), "%s", s_id);
  if (!s_session && !s_stopping) {
    const auto& cfg = device().app->config();
    if (cfg.remoteId[0])
      snprintf(id, sizeof(id), "%s", cfg.remoteId);
    else {
      uint8_t mac[6]{};
      esp_read_mac(mac, ESP_MAC_WIFI_STA);
      snprintf(id, sizeof(id), "gm-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
  }
  const char* state = s_session ? (s_session->online ? "online" : "connecting")
                                : (s_pending ? "connecting" : "idle");
  w.key("remote")
      .beginObject()
      .kv("state", state)
      .kv("id", id)
      .kvu("since", s_since)
      .kvu("until", s_until)
      .kvu("requests", s_requests)
      .kvu("rejected", s_rejected.load())
      .kv("last_error", s_error)
      .kvu("budget_min", device().app->config().remoteBudgetMin)
      .kvu("budget_left_s", device().app->remoteBudgetLeftS())
      .endObject();
}
bool remoteConsole(const char* line, void (*out)(void*, const char*),
                   void* ctx) {
  if (strncmp(line, "remote", 6) != 0 || (line[6] && line[6] != ' '))
    return false;
  if (strcmp(line, "remote stop") == 0) {
    remoteStop();
    out(ctx, "remote stopping\n");
  } else if (strncmp(line, "remote start ", 13) == 0) {
    unsigned minutes = 0;
    char extra;
    if (sscanf(line + 13, "%u %c", &minutes, &extra) != 1 || minutes < 1 ||
        minutes > 60) {
      out(ctx, "usage: remote start <1..60>\n");
    } else {
      out(ctx,
          request(minutes) ? "remote requested\n" : "remote unavailable\n");
    }
  } else if (strcmp(line, "remote") == 0) {
    char json[512];
    gm::JsonWriter w(json, sizeof(json));
    w.beginObject();
    remoteWriteJson(w);
    w.endObject();
    out(ctx, json);
    out(ctx, "\n");
  } else
    out(ctx, "usage: remote [start <1..60>|stop]\n");
  return true;
}
