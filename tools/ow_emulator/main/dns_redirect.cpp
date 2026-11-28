// SPDX-License-Identifier: MIT
// Standalone adaptation of firmware/service/dns_redirect and
// core/comm/dns_reply.
#include "dns_redirect.h"

#include <cerrno>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace {
const char* kTag = "dns";
int s_socket = -1;
uint32_t s_ip = 0;

int buildReply(const uint8_t* req, int len, uint8_t* out, int cap) {
  if (len < 12 || len > cap - 16) return 0;
  const uint16_t flags = static_cast<uint16_t>((req[2] << 8) | req[3]);
  const uint16_t questions = static_cast<uint16_t>((req[4] << 8) | req[5]);
  if ((flags & 0x8000) || ((flags >> 11) & 15) || questions != 1) return 0;
  int pos = 12;
  while (true) {
    if (pos >= len) return 0;
    const uint8_t label = req[pos];
    if (!label) {
      ++pos;
      break;
    }
    if ((label & 0xc0) || pos + 1 + label > len) return 0;
    pos += 1 + label;
  }
  if (pos + 4 > len) return 0;
  const uint16_t type = static_cast<uint16_t>((req[pos] << 8) | req[pos + 1]);
  const bool answer = type == 1 && req[pos + 2] == 0 && req[pos + 3] == 1;
  const int end = pos + 4;
  std::memcpy(out, req, end);
  out[2] = 0x80 | (req[2] & 1);
  out[3] = 0x80;
  out[6] = 0;
  out[7] = answer ? 1 : 0;
  out[8] = out[9] = out[10] = out[11] = 0;
  if (!answer) return end;
  const uint8_t record[16] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0,
                              0,    0,    0, 4, 0, 0, 0, 0};
  std::memcpy(out + end, record, sizeof(record));
  std::memcpy(out + end + 12, &s_ip, 4);
  return end + 16;
}

void dnsTask(void*) {
  uint8_t req[512];
  uint8_t resp[528];
  while (true) {
    sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    const int len = lwip_recvfrom(s_socket, req, sizeof(req), 0,
                                  reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (len < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        ESP_LOGW(kTag, "recv: errno=%d", errno);
      continue;
    }
    const int n = buildReply(req, len, resp, sizeof(resp));
    if (n > 0 && lwip_sendto(s_socket, resp, n, 0,
                             reinterpret_cast<sockaddr*>(&from), fromLen) < 0)
      ESP_LOGW(kTag, "send: errno=%d", errno);
  }
}
}  // namespace

esp_err_t dnsRedirectStart(uint32_t ip) {
  s_ip = ip;
  s_socket = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_socket < 0) return ESP_FAIL;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  timeval timeout = {1, 0};
  if (lwip_setsockopt(s_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                      sizeof(timeout)) != 0 ||
      lwip_bind(s_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
          0) {
    ESP_LOGE(kTag, "bind/socket option failed: errno=%d", errno);
    lwip_close(s_socket);
    s_socket = -1;
    return ESP_FAIL;
  }
  if (xTaskCreate(dnsTask, "dns", 3072, nullptr, 3, nullptr) != pdPASS) {
    lwip_close(s_socket);
    s_socket = -1;
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kTag, "captive DNS started");
  return ESP_OK;
}
