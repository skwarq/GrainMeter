// SPDX-License-Identifier: MIT
#pragma once

#include "app/platform.h"
#include "esp_http_client.h"

// HTTPS with esp_http_client. Server certificates are verified against the
// ESP-IDF CA bundle; plain http:// is refused. Requests of one upload window
// reuse a single keep-alive connection (one TLS handshake) until
// endSession(), which saves radio-on time on battery.
class HttpsClient : public gm::HttpClient {
 public:
  gm::HttpResult postJson(const char* url, const char* body, size_t len,
                          uint32_t timeoutMs, char* resp,
                          size_t respCap) override;
  gm::HttpResult get(const char* url, uint32_t timeoutMs, char* resp,
                     size_t respCap) override;
  gm::HttpResult download(const char* url, uint32_t timeoutMs,
                          gm::DownloadSink& sink) override;
  void endSession() override;

 private:
  esp_http_client_handle_t prepare(const char* url, esp_http_client_method_t m,
                                   uint32_t timeoutMs, gm::HttpResult* res);
  void dropConnection();
  int64_t openGet(esp_http_client_handle_t c, gm::HttpResult* res);
  // Date header of the current response (clock correction), 0 = none.
  static esp_err_t onEvent(esp_http_client_event_t* evt);
  uint32_t dateEpoch_ = 0;

  esp_http_client_handle_t client_ = nullptr;
  char host_[96] = "";
};
