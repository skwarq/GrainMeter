// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstdlib>

#include "dns_reply.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "services.h"

namespace ow {
namespace {
uint32_t ap_ip;

void dnsTask(void*) {
  const int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE("dns", "socket failed: errno=%d", errno);
    abort();
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  timeval timeout{1, 0};
  if (lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                      sizeof(timeout)) ||
      lwip_bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) {
    ESP_LOGE("dns", "socket setup failed: errno=%d", errno);
    lwip_close(sock);
    abort();
  }
  uint8_t request[512];
  uint8_t response[528];
  ESP_LOGI("dns", "captive DNS listening on :53");
  while (true) {
    sockaddr_in from{};
    socklen_t size = sizeof(from);
    const int len = lwip_recvfrom(sock, request, sizeof(request), 0,
                                  reinterpret_cast<sockaddr*>(&from), &size);
    if (len < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        ESP_LOGE("dns", "recvfrom: errno=%d", errno);
      continue;
    }
    const int n =
        dnsBuildReply(request, len, ap_ip, response, sizeof(response));
    if (n > 0 && lwip_sendto(sock, response, static_cast<size_t>(n), 0,
                             reinterpret_cast<sockaddr*>(&from), size) < 0)
      ESP_LOGE("dns", "sendto: errno=%d", errno);
  }
}
}  // namespace

esp_err_t dnsStart(uint32_t ip) {
  ap_ip = ip;
  return xTaskCreate(dnsTask, "dns", 3072, nullptr, 3, nullptr) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}
}  // namespace ow
