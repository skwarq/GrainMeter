// SPDX-License-Identifier: MIT
#include "service/udp_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

// The log hook runs in whatever task logs (some have small stacks, e.g. the
// Wi-Fi driver): it only formats into a queue. A sender task owns the socket.
static vprintf_like_t s_prev = nullptr;
static RingbufHandle_t s_queue = nullptr;
static SemaphoreHandle_t s_fmtLock = nullptr;
static TaskHandle_t s_task = nullptr;
static volatile bool s_enabled = false;
static sockaddr_in s_dst = {};
static char s_prefix[33] = "";
static volatile uint32_t s_dropped = 0;
static volatile uint32_t s_sent = 0;
static RingbufHandle_t volatile s_tap = nullptr;  // LAN console session

static int logHook(const char* fmt, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  const int r = s_prev != nullptr ? s_prev(fmt, ap) : vprintf(fmt, ap);
  // Non-blocking: a log line from inside the sender path (lwIP) or a
  // concurrent writer is simply not forwarded.
  if ((s_enabled || s_tap != nullptr) && s_fmtLock != nullptr &&
      xSemaphoreTake(s_fmtLock, 0) == pdTRUE) {
    // A log line may arrive in several vprintf calls (log v2): prefix only
    // the first fragment of each line.
    static char line[256];
    static bool atLineStart = true;
    int n = atLineStart ? snprintf(line, sizeof(line), "%s ", s_prefix) : 0;
    if (n < 0) n = 0;
    const int prefixLen = n;
    const int m = vsnprintf(line + n, sizeof(line) - n, fmt, cp);
    if (m > 0) {
      n += m;
      if (n >= static_cast<int>(sizeof(line))) n = sizeof(line) - 1;
      atLineStart = line[n - 1] == '\n';
      if (s_enabled &&
          xRingbufferSend(s_queue, line, static_cast<size_t>(n), 0) != pdTRUE) {
        s_dropped = s_dropped + 1;
      }
      RingbufHandle_t tap = s_tap;
      if (tap != nullptr && n > prefixLen) {  // no device prefix on the console
        xRingbufferSend(tap, line + prefixLen,
                        static_cast<size_t>(n - prefixLen), 0);
      }
    }
    xSemaphoreGive(s_fmtLock);
  }
  va_end(cp);
  return r;
}

static void senderTask(void*) {
  int sock = -1;
  while (true) {
    size_t len = 0;
    char* item = static_cast<char*>(
        xRingbufferReceive(s_queue, &len, pdMS_TO_TICKS(1000)));
    if (item == nullptr) {
      continue;
    }
    if (s_enabled) {
      if (sock < 0) {
        sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock >= 0) {
          const int yes = 1;
          lwip_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        }
      }
      if (sock >= 0) {
        const sockaddr_in dst = s_dst;
        // Fails quietly without a station link (sleep cycle, AP only).
        if (lwip_sendto(sock, item, len, MSG_DONTWAIT,
                        reinterpret_cast<const sockaddr*>(&dst),
                        sizeof(dst)) > 0) {
          s_sent = s_sent + 1;
        }
      }
    }
    vRingbufferReturnItem(s_queue, item);
  }
}

static esp_log_level_t espLevel(uint32_t level) {
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

void udpLogApply(bool enabled, uint32_t level, const char* host, uint16_t port,
                 const char* prefix) {
  if (s_queue == nullptr) {
    s_queue = xRingbufferCreate(6144, RINGBUF_TYPE_NOSPLIT);
    s_fmtLock = xSemaphoreCreateMutex();
    if (s_queue == nullptr || s_fmtLock == nullptr) {
      return;
    }
    xTaskCreate(senderTask, "udplog", 3072, nullptr, 2, &s_task);
    s_prev = esp_log_set_vprintf(logHook);
  }
  sockaddr_in dst = {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port);
  dst.sin_addr.s_addr =
      host != nullptr && host[0] != '\0' ? inet_addr(host) : INADDR_BROADCAST;
  s_dst = dst;
  snprintf(s_prefix, sizeof(s_prefix), "%s", prefix != nullptr ? prefix : "");
  const bool wasEnabled = s_enabled;
  s_enabled = enabled;
  esp_log_level_set("*", enabled ? espLevel(level) : ESP_LOG_INFO);
  // At debug level the HTTP/TLS stack logs request URLs; the Telegram URL
  // contains the bot token and UDP logs are broadcast on the LAN.
  if (enabled && level >= 4) {
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_INFO);
    esp_log_level_set("transport_base", ESP_LOG_INFO);
  }
  if (enabled && !wasEnabled) {
    ESP_LOGI("udplog", "forwarding logs to %s:%u",
             host != nullptr && host[0] != '\0' ? host : "255.255.255.255",
             static_cast<unsigned>(port));
  }
}

// Swapped under the format lock: once this returns, no log hook still uses
// the previous ring buffer, so the caller may delete it.
void logTapSet(RingbufHandle_t rb) {
  if (s_fmtLock != nullptr) xSemaphoreTake(s_fmtLock, portMAX_DELAY);
  s_tap = rb;
  if (s_fmtLock != nullptr) xSemaphoreGive(s_fmtLock);
}

uint32_t udpLogDropped() { return s_dropped; }
uint32_t udpLogSent() { return s_sent; }
