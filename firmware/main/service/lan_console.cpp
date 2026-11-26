// SPDX-License-Identifier: MIT
#include "service/lan_console.h"

#include <cstdio>
#include <cstring>

#include "app/shell.h"
#include "device.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "service/udp_log.h"

static const char* kTag = "lan";
static const uint16_t kHelloPort = 5515;
static const uint16_t kTcpPort = 5516;
static const int64_t kIdleUs = 60LL * 1000000;
static const int64_t kMaxSessionUs = 4LL * 3600 * 1000000;
static const int kMaxAuthTries = 3;

static int s_listen = -1;
static volatile bool s_active = false;  // a client is connected
static volatile bool s_authed = false;  // ... and logged in: holds the device
static const char* volatile s_endReason = nullptr;
static int64_t s_lastHelloUs = 0;
static char s_ssid[33] = "";

// Survives software restarts (not power loss).
RTC_NOINIT_ATTR static uint32_t s_resumeMagic;
static const uint32_t kResumeMagic = 0x4C414E52;  // "LANR"

// ---------------------------------------------------------------- output ----

static void sendAll(int sock, const char* p, size_t n) {
  while (n > 0) {
    const int w = lwip_send(sock, p, n, 0);
    if (w <= 0) return;  // closed or send timeout: the loop notices later
    p += w;
    n -= static_cast<size_t>(w);
  }
}

// "\n" -> "\r\n" like the USB console.
static void sendText(int sock, const char* p, size_t len) {
  const char* end = p + len;
  while (p < end) {
    const char* nl = static_cast<const char*>(
        std::memchr(p, '\n', static_cast<size_t>(end - p)));
    const size_t n = nl != nullptr ? static_cast<size_t>(nl - p)
                                   : static_cast<size_t>(end - p);
    if (n > 0) sendAll(sock, p, n);
    if (nl == nullptr) break;
    sendAll(sock, "\r\n", 2);
    p = nl + 1;
  }
}

// Shell writer.
static void tcpOut(void* ctx, const char* text) {
  sendText(static_cast<int>(reinterpret_cast<intptr_t>(ctx)), text,
           std::strlen(text));
}

// ------------------------------------------------------------------ hello ----

static void appendEncoded(char* out, size_t cap, size_t* n, const char* s) {
  static const char kHex[] = "0123456789ABCDEF";
  for (; *s != '\0' && *n + 4 < cap; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                       c == '.' || c == '~';
    if (plain) {
      out[(*n)++] = static_cast<char>(c);
    } else {
      out[(*n)++] = '%';
      out[(*n)++] = kHex[c >> 4];
      out[(*n)++] = kHex[c & 15];
    }
  }
  out[*n] = '\0';
}

static void sendHello(const LanAnnounce& a, uint32_t windowMs) {
  char msg[256];
  size_t n = 0;
  const struct {
    const char* key;
    const char* value;
  } fields[] = {{"ssid", a.ssid}, {"name", a.name}, {"ip", a.ip}, {"fw", a.fw}};
  // Fixed fields first, values last: a long name is cut, never the buffer.
  int w = snprintf(msg, sizeof(msg), "GM-HELLO v1 port=%u window_ms=%lu",
                   static_cast<unsigned>(kTcpPort),
                   static_cast<unsigned long>(windowMs));
  n = w > 0 ? static_cast<size_t>(w) : 0;
  for (const auto& f : fields) {
    if (n + 8 + std::strlen(f.key) >= sizeof(msg) - 2) break;
    w = snprintf(msg + n, sizeof(msg) - n, " %s=", f.key);
    if (w <= 0) break;
    n += static_cast<size_t>(w);
    appendEncoded(msg, sizeof(msg) - 2, &n, f.value != nullptr ? f.value : "");
  }
  msg[n++] = '\n';
  msg[n] = '\0';
  const int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) return;
  const int yes = 1;
  lwip_setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  sockaddr_in dst = {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(kHelloPort);
  dst.sin_addr.s_addr = INADDR_BROADCAST;
  lwip_sendto(sock, msg, n, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
  lwip_close(sock);
  s_lastHelloUs = esp_timer_get_time();
  snprintf(s_ssid, sizeof(s_ssid), "%s", a.ssid != nullptr ? a.ssid : "");
}

// ---------------------------------------------------------------- session ----

static void sessionTask(void* arg) {
  const int sock = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  void* ctx = reinterpret_cast<void*>(static_cast<intptr_t>(sock));
  timeval tv = {2, 0};
  lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  const int yes = 1;
  lwip_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  char banner[96];
  snprintf(banner, sizeof(banner), "GM-CONSOLE v1 ssid=%s fw=%s\nAUTH?\n",
           s_ssid, esp_app_get_description()->version);
  tcpOut(ctx, banner);

  gm::Shell* shell = nullptr;
  RingbufHandle_t tap = nullptr;
  bool authed = false;
  bool logsOn = true;
  int tries = 0;
  const char* bye = nullptr;
  char line[gm::kShellMaxLine];
  size_t len = 0;
  const int64_t startUs = esp_timer_get_time();
  int64_t lastRxUs = startUs;

  while (bye == nullptr) {
    if (s_endReason != nullptr) {
      bye = s_endReason;
      break;
    }
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(sock, &rd);
    timeval wait = {0, 100000};
    const int ready = lwip_select(sock + 1, &rd, nullptr, nullptr, &wait);
    if (ready > 0) {
      char buf[128];
      const int n = lwip_recv(sock, buf, sizeof(buf), 0);
      if (n <= 0) {
        bye = "closed";
        break;
      }
      for (int i = 0; i < n && bye == nullptr; ++i) {
        const char c = buf[i];
        if (c == '\r') continue;
        if (c != '\n') {
          if (len + 1 < sizeof(line)) line[len++] = c;
          continue;
        }
        line[len] = '\0';
        len = 0;
        lastRxUs = esp_timer_get_time();
        if (!authed) {
          bool ok = false;
          if (std::strncmp(line, "auth ", 5) == 0) {
            DeviceLock lock;
            ok = device().portal->authorized(line + 5);
          }
          std::memset(line, 0, sizeof(line));
          if (ok) {
            authed = true;
            s_authed = true;
            shell = new gm::Shell(*device().app, tcpOut, ctx);
            tap = xRingbufferCreate(4096, RINGBUF_TYPE_NOSPLIT);
            if (tap != nullptr) logTapSet(tap);
            tcpOut(ctx, "AUTH OK\n> ");
            ESP_LOGI(kTag, "console session started");
          } else if (++tries >= kMaxAuthTries) {
            bye = "auth";
          } else {
            tcpOut(ctx, "AUTH FAIL\nAUTH?\n");
          }
          continue;
        }
        if (std::strcmp(line, "#ping") == 0) {
          tcpOut(ctx, "#pong\n");
        } else if (std::strcmp(line, "exit") == 0 ||
                   std::strcmp(line, "quit") == 0) {
          bye = "exit";
        } else if (std::strcmp(line, "logs off") == 0 ||
                   std::strcmp(line, "logs on") == 0) {
          logsOn = line[6] == 'n';
          logTapSet(logsOn ? tap : nullptr);
          tcpOut(ctx, logsOn ? "logs on\n> " : "logs off\n> ");
        } else {
          bool restart;
          {
            DeviceLock lock;
            shell->exec(line);
            restart = shell->restartRequested();
          }
          if (restart) {
            device().sys.restart = true;  // main loop: BYE restart, resume
          }
          tcpOut(ctx, "> ");
        }
      }
    } else if (ready < 0) {
      bye = "closed";
      break;
    }
    // Live log lines (only this task writes to the socket).
    if (tap != nullptr) {
      size_t n = 0;
      void* item;
      while ((item = xRingbufferReceive(tap, &n, 0)) != nullptr) {
        sendText(sock, static_cast<const char*>(item), n);
        vRingbufferReturnItem(tap, item);
      }
    }
    const int64_t now = esp_timer_get_time();
    if (!authed && now - startUs > 10LL * 1000000) bye = "auth";
    if (now - lastRxUs > kIdleUs) bye = "idle";
    if (now - startUs > kMaxSessionUs) bye = "timeout";
  }

  if (std::strcmp(bye, "closed") != 0) {
    char msg[40];
    snprintf(msg, sizeof(msg), "BYE %s\n", bye);
    tcpOut(ctx, msg);
  }
  ESP_LOGI(kTag, "console session ended: %s", bye);
  logTapSet(nullptr);  // waits for a running log hook
  if (tap != nullptr) vRingbufferDelete(tap);
  delete shell;
  lwip_shutdown(sock, SHUT_RDWR);
  lwip_close(sock);
  s_endReason = nullptr;
  s_authed = false;
  s_active = false;
  vTaskDelete(nullptr);
}

// ----------------------------------------------------------------- server ----

static bool openServer() {
  if (s_listen >= 0) return true;
  s_listen = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s_listen < 0) return false;
  const int yes = 1;
  lwip_setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kTcpPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (lwip_bind(s_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
          0 ||
      lwip_listen(s_listen, 1) != 0) {
    ESP_LOGW(kTag, "TCP :%u not available", static_cast<unsigned>(kTcpPort));
    lwip_close(s_listen);
    s_listen = -1;
    return false;
  }
  lwip_fcntl(s_listen, F_SETFL, O_NONBLOCK);
  return true;
}

void lanServerClose() {
  if (s_listen >= 0) {
    lwip_close(s_listen);
    s_listen = -1;
  }
}

// Accepts one pending client (non-blocking) and starts its session.
static bool acceptClient() {
  if (s_listen < 0 || s_active) return false;
  sockaddr_in from = {};
  socklen_t fromLen = sizeof(from);
  const int c =
      lwip_accept(s_listen, reinterpret_cast<sockaddr*>(&from), &fromLen);
  if (c < 0) return false;
  ESP_LOGI(kTag, "console client %s", inet_ntoa(from.sin_addr));
  s_endReason = nullptr;
  s_authed = false;
  s_active = true;
  if (xTaskCreate(sessionTask, "lan", 6144,
                  reinterpret_cast<void*>(static_cast<intptr_t>(c)), 3,
                  nullptr) != pdPASS) {
    lwip_close(c);
    s_active = false;
    return false;
  }
  return true;
}

bool lanOffer(const LanAnnounce& a, uint32_t windowMs) {
  if (s_active) return true;
  if (!openServer()) return false;
  sendHello(a, windowMs);
  const int64_t end =
      esp_timer_get_time() + static_cast<int64_t>(windowMs) * 1000;
  while (esp_timer_get_time() < end) {
    if (acceptClient()) {
      // Only a logged-in client holds the device awake (a port scan or a
      // silent connection must not).
      const int64_t authEnd = esp_timer_get_time() + 3000000;
      while (s_active && !s_authed && esp_timer_get_time() < authEnd) {
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      if (s_authed) return true;
      lanSessionEnd("auth");
      lanServerClose();
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  lanServerClose();
  return false;
}

void lanPoll(const LanAnnounce& a, uint32_t helloEveryMs) {
  if (s_active) return;
  if (!openServer()) return;
  if (esp_timer_get_time() - s_lastHelloUs >=
      static_cast<int64_t>(helloEveryMs) * 1000) {
    sendHello(a, helloEveryMs + 1000);
  }
  acceptClient();
}

bool lanSessionActive() { return s_active && s_authed; }

void lanSessionEnd(const char* reason) {
  if (!s_active) return;
  s_endReason = reason;
  for (int i = 0; i < 30 && s_active; ++i) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void lanMarkResume() { s_resumeMagic = kResumeMagic; }

bool lanTakeResume() {
  const bool r = s_resumeMagic == kResumeMagic;
  s_resumeMagic = 0;
  return r;
}
