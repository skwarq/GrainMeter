// SPDX-License-Identifier: MIT
#include "service/dns_redirect.h"

#include <cstring>

#include "comm/dns_reply.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* kTag = "dns";
static volatile bool s_running = false;
static TaskHandle_t s_task = nullptr;
static uint32_t s_ip = 0;

static void dnsTask(void*) {
  const int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(kTag, "socket failed");
    s_running = false;
  } else {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    timeval tv = {1, 0};
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (lwip_bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
      ESP_LOGE(kTag, "bind :53 failed");
      s_running = false;
    } else {
      ESP_LOGI(kTag, "captive DNS started");
    }
    uint8_t req[512];
    uint8_t resp[528];
    while (s_running) {
      sockaddr_in from = {};
      socklen_t fromLen = sizeof(from);
      const int len =
          lwip_recvfrom(sock, req, sizeof(req), 0,
                        reinterpret_cast<sockaddr*>(&from), &fromLen);
      if (len <= 0) {
        continue;  // timeout: check s_running
      }
      const int n = gm::dnsBuildReply(req, len, s_ip, resp, sizeof(resp));
      if (n > 0) {
        lwip_sendto(sock, resp, static_cast<size_t>(n), 0,
                    reinterpret_cast<sockaddr*>(&from), fromLen);
      }
    }
    lwip_close(sock);
  }
  ESP_LOGI(kTag, "captive DNS stopped");
  s_task = nullptr;
  vTaskDelete(nullptr);
}

bool dnsRedirectStart(uint32_t ip) {
  if (s_task != nullptr) {
    return true;
  }
  s_ip = ip;
  s_running = true;
  return xTaskCreate(dnsTask, "dns", 3072, nullptr, 4, &s_task) == pdPASS;
}

void dnsRedirectStop() {
  s_running = false;
  for (int i = 0; i < 30 && s_task != nullptr; ++i) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
