// SPDX-License-Identifier: MIT
#include "service/udp_log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "service/remote_session.h"
#include "util/log_buffer.h"

// All formatting/queue storage is static: Wi-Fi and MQTT callers have small
// stacks. Complete lines occupy at most 4 KiB; saturation evicts the oldest.
namespace {
constexpr size_t kLineSize = 256;
constexpr size_t kLines = 16;
struct Line {
  char text[kLineSize];
  uint16_t len;
};
Line s_lines[kLines];
size_t s_head = 0, s_count = 0;
char s_partial[kLineSize];
size_t s_partialLen = 0;
vprintf_like_t s_prev = nullptr;
SemaphoreHandle_t s_fmtLock = nullptr;
TaskHandle_t s_task = nullptr;
bool s_enabled = false, s_mqtt = false;
sockaddr_in s_dst = {};
char s_prefix[33] = "";
std::atomic<uint32_t> s_udpDropped{0}, s_udpSent{0};
std::atomic<uint32_t> s_mqttDropped{0}, s_mqttSent{0};
uint32_t s_pendingDropped = 0;
std::atomic<bool> s_publishing{false};
RingbufHandle_t s_tap = nullptr;
gm::LogBuffer* s_preview = nullptr;
SemaphoreHandle_t s_previewLock = nullptr;
uint64_t s_previewLast = 0;
int64_t s_previewReadUs = 0;

class PreviewSource : public gm::LogSource {
 public:
  bool read(uint64_t since, char* out, size_t cap) override {
    if (!s_fmtLock) return false;
    xSemaphoreTake(s_fmtLock, portMAX_DELAY);
    if (!s_previewLock) s_previewLock = xSemaphoreCreateMutex();
    const SemaphoreHandle_t lock = s_previewLock;
    xSemaphoreGive(s_fmtLock);
    if (!lock) return false;
    // JSON serialization must not hold the UDP/MQTT/LAN formatting lock.
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!s_preview) s_preview = new (std::nothrow) gm::LogBuffer(s_previewLast);
    s_previewReadUs = esp_timer_get_time();
    const bool ok = s_preview && s_preview->read(since, out, cap);
    xSemaphoreGive(lock);
    return ok;
  }
};
PreviewSource s_previewSource;

void dropped(uint32_t count) {
  if (s_mqtt) {
    s_mqttDropped.fetch_add(count);
    s_pendingDropped += count;
  } else {
    s_udpDropped.fetch_add(count);
  }
}

void queueLine() {
  if (s_count == kLines) {
    s_head = (s_head + 1) % kLines;
    --s_count;
    dropped(1);
  }
  Line& line = s_lines[(s_head + s_count) % kLines];
  memcpy(line.text, s_partial, s_partialLen);
  line.len = static_cast<uint16_t>(s_partialLen);
  ++s_count;
  s_partialLen = 0;
  if (!s_mqtt) xTaskNotifyGive(s_task);
}

int logHook(const char* fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  const int result = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
  if (s_fmtLock && xSemaphoreTake(s_fmtLock, 0) == pdTRUE) {
    static char fragment[kLineSize];
    const int n = vsnprintf(fragment, sizeof(fragment), fmt, cp);
    if (n > 0) {
      const size_t len = n < static_cast<int>(sizeof(fragment))
                             ? static_cast<size_t>(n)
                             : sizeof(fragment) - 1;
      if (s_tap) xRingbufferSend(s_tap, fragment, len, 0);
      if (s_previewLock && xSemaphoreTake(s_previewLock, 0) == pdTRUE) {
        if (s_preview)
          s_preview->append(
              fragment, len,
              n >= static_cast<int>(sizeof(fragment)) && strchr(fmt, '\n'));
        xSemaphoreGive(s_previewLock);
      }
      // Synchronous logs produced by publish must not feed that same sender.
      // Other tasks (and the LAN tap) are unaffected.
      if (s_enabled && !(s_mqtt && s_publishing.load() &&
                         xTaskGetCurrentTaskHandle() == s_task)) {
        for (size_t i = 0; i < len; ++i) {
          if (!s_partialLen) {
            const int prefix =
                snprintf(s_partial, sizeof(s_partial), "%s ", s_prefix);
            s_partialLen = prefix > 0 ? static_cast<size_t>(prefix) : 0;
          }
          if (s_partialLen < kLineSize - 1)
            s_partial[s_partialLen++] = fragment[i];
          if (fragment[i] == '\n') {
            // Truncated lines still terminate, never merge with the next line.
            if (s_partial[s_partialLen - 1] != '\n')
              s_partial[s_partialLen - 1] = '\n';
            queueLine();
          }
        }
        // Legacy log v1 can format a whole long line in a single call.
        if (n >= static_cast<int>(sizeof(fragment)) && strchr(fmt, '\n')) {
          if (s_partialLen) {
            s_partial[s_partialLen - 1] = '\n';
            queueLine();
          }
        }
      }
    }
    xSemaphoreGive(s_fmtLock);
  }
  va_end(cp);
  return result;
}

void senderTask(void*) {
  int sock = -1;
  static char batch[1024];
  while (true) {
    xSemaphoreTake(s_fmtLock, portMAX_DELAY);
    if (s_previewLock && xSemaphoreTake(s_previewLock, 0) == pdTRUE) {
      if (s_preview &&
          esp_timer_get_time() - s_previewReadUs >= 300LL * 1000000) {
        s_previewLast = s_preview->last();
        delete s_preview;
        s_preview = nullptr;
      }
      xSemaphoreGive(s_previewLock);
    }
    const bool enabled = s_enabled, mqtt = s_mqtt;
    xSemaphoreGive(s_fmtLock);
    if (mqtt)
      vTaskDelay(pdMS_TO_TICKS(250));
    else
      ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(250));
    if (!enabled || (mqtt && !remoteLogOnline())) continue;

    xSemaphoreTake(s_fmtLock, portMAX_DELAY);
    if (!s_enabled || s_mqtt != mqtt) {
      xSemaphoreGive(s_fmtLock);
      continue;
    }
    size_t len = 0;
    uint32_t lines = 0, reported = 0;
    if (mqtt && s_pendingDropped) {
      reported = s_pendingDropped;
      len = static_cast<size_t>(snprintf(batch, sizeof(batch),
                                         "... %lu lines dropped\n",
                                         static_cast<unsigned long>(reported)));
      s_pendingDropped = 0;
    }
    while (s_count) {
      const Line& line = s_lines[s_head];
      if (len + line.len > sizeof(batch)) break;
      memcpy(batch + len, line.text, line.len);
      len += line.len;
      ++lines;
      s_head = (s_head + 1) % kLines;
      --s_count;
      if (!mqtt) break;
    }
    const sockaddr_in dst = s_dst;
    xSemaphoreGive(s_fmtLock);
    if (!len) continue;

    bool sent = false;
    if (mqtt) {
      s_publishing = true;
      sent = remoteLogPublish(batch, len);
      s_publishing = false;
      if (sent)
        s_mqttSent.fetch_add(lines);
      else {
        s_mqttDropped.fetch_add(lines);
        xSemaphoreTake(s_fmtLock, portMAX_DELAY);
        s_pendingDropped += lines + reported;
        xSemaphoreGive(s_fmtLock);
      }
    } else {
      if (sock < 0) {
        sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock >= 0) {
          const int yes = 1;
          lwip_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        }
      }
      sent = sock >= 0 && lwip_sendto(sock, batch, len, MSG_DONTWAIT,
                                      reinterpret_cast<const sockaddr*>(&dst),
                                      sizeof(dst)) > 0;
      if (sent)
        s_udpSent.fetch_add(lines);
      else
        s_udpDropped.fetch_add(lines);
    }
  }
}

esp_log_level_t espLevel(uint32_t level) {
  switch (level) {
    case 1:
      return ESP_LOG_ERROR;
    case 2:
      return ESP_LOG_WARN;
    case 4:
      return ESP_LOG_DEBUG;
    default:
      return ESP_LOG_INFO;
  }
}
}  // namespace

void udpLogApply(bool enabled, uint32_t level, const char* host, uint16_t port,
                 const char* prefix, bool mqtt) {
  if (!s_fmtLock) {
    s_fmtLock = xSemaphoreCreateMutex();
    if (!s_fmtLock) {
      ESP_LOGE("udplog", "cannot allocate log mutex");
      return;
    }
    if (xTaskCreate(senderTask, "udplog", 3072, nullptr, 2, &s_task) !=
        pdPASS) {
      vSemaphoreDelete(s_fmtLock);
      s_fmtLock = nullptr;
      ESP_LOGE("udplog", "cannot start log sender");
      return;
    }
    s_prev = esp_log_set_vprintf(logHook);
  }
  sockaddr_in dst = {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port);
  dst.sin_addr.s_addr = host && host[0] ? inet_addr(host) : INADDR_BROADCAST;
  xSemaphoreTake(s_fmtLock, portMAX_DELAY);
  if (s_mqtt != mqtt || (s_enabled && !enabled)) {
    dropped(static_cast<uint32_t>(s_count));
    s_count = s_head = s_partialLen = 0;
    s_pendingDropped = 0;
  }
  s_dst = dst;
  snprintf(s_prefix, sizeof(s_prefix), "%s", prefix ? prefix : "");
  s_mqtt = mqtt;
  s_enabled = enabled;
  xSemaphoreGive(s_fmtLock);
  // The level also applies with sending off: the portal view (/api/logs)
  // shows the same lines.
  esp_log_level_set("*", espLevel(level));
  // Protect URL credentials at debug and suppress asynchronous MQTT/TLS
  // debug/info chatter, including publish logs, to prevent feedback.
  const esp_log_level_t globalLevel = espLevel(level);
  esp_log_level_set("HTTP_CLIENT",
                    enabled && level >= 4 ? ESP_LOG_INFO : globalLevel);
  const esp_log_level_t transportLevel =
      enabled && mqtt         ? (level == 1 ? ESP_LOG_ERROR : ESP_LOG_WARN)
      : enabled && level >= 4 ? ESP_LOG_INFO
                              : globalLevel;
  esp_log_level_set("mqtt_client", transportLevel);
  esp_log_level_set("esp-tls", transportLevel);
  esp_log_level_set("transport_base", transportLevel);
  esp_log_level_set("transport", transportLevel);
  esp_log_level_set("transport_ws", transportLevel);
  esp_log_level_set("transport_ssl", transportLevel);
}

void logTapSet(RingbufHandle_t rb) {
  if (s_fmtLock) xSemaphoreTake(s_fmtLock, portMAX_DELAY);
  s_tap = rb;
  if (s_fmtLock) xSemaphoreGive(s_fmtLock);
}

gm::LogSource* logPreviewSource() { return &s_previewSource; }

uint32_t udpLogDropped() { return s_udpDropped.load(); }
uint32_t udpLogSent() { return s_udpSent.load(); }
uint32_t mqttLogDropped() { return s_mqttDropped.load(); }
uint32_t mqttLogSent() { return s_mqttSent.load(); }
bool logDestinationMqtt() {
  if (s_fmtLock) xSemaphoreTake(s_fmtLock, portMAX_DELAY);
  const bool mqtt = s_mqtt;
  if (s_fmtLock) xSemaphoreGive(s_fmtLock);
  return mqtt;
}
