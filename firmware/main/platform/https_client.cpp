// SPDX-License-Identifier: MIT
#include "platform/https_client.h"

#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char* kTag = "https";

static gm::HttpResult emptyResult() {
  gm::HttpResult r;
  r.transportOk = false;
  r.status = -1;
  r.error[0] = '\0';
  return r;
}

// "https://host[:port]/..." -> "host[:port]"
static bool hostOf(const char* url, char* out, size_t cap) {
  if (std::strncmp(url, "https://", 8) != 0) return false;
  const char* h = url + 8;
  const char* end = std::strchr(h, '/');
  const size_t n =
      end != nullptr ? static_cast<size_t>(end - h) : std::strlen(h);
  if (n == 0 || n >= cap) return false;
  std::memcpy(out, h, n);
  out[n] = '\0';
  return true;
}

void HttpsClient::dropConnection() {
  if (client_ != nullptr) {
    esp_http_client_close(client_);
    esp_http_client_cleanup(client_);
    client_ = nullptr;
  }
  host_[0] = '\0';
}

void HttpsClient::endSession() { dropConnection(); }

esp_http_client_handle_t HttpsClient::prepare(const char* url,
                                              esp_http_client_method_t method,
                                              uint32_t timeoutMs,
                                              gm::HttpResult* res) {
  char host[sizeof(host_)];
  if (!hostOf(url, host, sizeof(host))) {
    snprintf(res->error, sizeof(res->error), "only https:// is allowed");
    return nullptr;
  }
  if (client_ != nullptr && std::strcmp(host, host_) != 0) {
    dropConnection();  // other server: new TLS session
  }
  if (client_ == nullptr) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = static_cast<int>(timeoutMs);
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // verification ON
    cfg.disable_auto_redirect = true;
    cfg.keep_alive_enable = true;
    cfg.buffer_size = 2048;
    client_ = esp_http_client_init(&cfg);
    if (client_ == nullptr) {
      snprintf(res->error, sizeof(res->error), "client init failed");
      return nullptr;
    }
    std::snprintf(host_, sizeof(host_), "%s", host);
  } else {
    esp_http_client_set_url(client_, url);
    esp_http_client_set_timeout_ms(client_, static_cast<int>(timeoutMs));
  }
  esp_http_client_set_method(client_, method);
  return client_;
}

gm::HttpResult HttpsClient::postJson(const char* url, const char* body,
                                     size_t len, uint32_t timeoutMs, char* resp,
                                     size_t respCap) {
  gm::HttpResult res = emptyResult();
  if (respCap > 0) resp[0] = '\0';
  esp_http_client_handle_t c = prepare(url, HTTP_METHOD_POST, timeoutMs, &res);
  if (c == nullptr) return res;
  esp_http_client_set_header(c, "Content-Type", "application/json");
  esp_err_t err = esp_http_client_open(c, static_cast<int>(len));
  if (err != ESP_OK) {
    snprintf(res.error, sizeof(res.error), "connect/TLS: %s",
             esp_err_to_name(err));
    dropConnection();
    ESP_LOGW(kTag, "POST failed: %s",
             res.error);  // body holds the key: not logged
    return res;
  }
  const int written = esp_http_client_write(c, body, static_cast<int>(len));
  if (written != static_cast<int>(len) ||
      esp_http_client_fetch_headers(c) < 0) {
    snprintf(res.error, sizeof(res.error), "request failed (%d of %u bytes)",
             written, static_cast<unsigned>(len));
    dropConnection();
    return res;
  }
  res.status = esp_http_client_get_status_code(c);
  int total = 0;
  while (respCap > 1 && total < static_cast<int>(respCap) - 1) {
    const int n = esp_http_client_read(c, resp + total,
                                       static_cast<int>(respCap) - 1 - total);
    if (n <= 0) break;
    total += n;
  }
  if (respCap > 0) resp[total] = '\0';
  int rest = 0;
  esp_http_client_flush_response(c, &rest);  // keep the connection reusable
  res.transportOk = true;
  return res;
}

gm::HttpResult HttpsClient::get(const char* url, uint32_t timeoutMs, char* resp,
                                size_t respCap) {
  gm::HttpResult res = emptyResult();
  if (respCap > 0) resp[0] = '\0';
  esp_http_client_handle_t c = prepare(url, HTTP_METHOD_GET, timeoutMs, &res);
  if (c == nullptr) return res;
  esp_err_t err = esp_http_client_open(c, 0);
  if (err != ESP_OK || esp_http_client_fetch_headers(c) < 0) {
    snprintf(res.error, sizeof(res.error), "GET: %s", esp_err_to_name(err));
    dropConnection();
    return res;
  }
  res.status = esp_http_client_get_status_code(c);
  int total = 0;
  while (respCap > 1 && total < static_cast<int>(respCap) - 1) {
    const int n = esp_http_client_read(c, resp + total,
                                       static_cast<int>(respCap) - 1 - total);
    if (n <= 0) break;
    total += n;
  }
  if (respCap > 0) resp[total] = '\0';
  int rest = 0;
  esp_http_client_flush_response(c, &rest);
  res.transportOk = true;
  return res;
}

gm::HttpResult HttpsClient::download(const char* url, uint32_t timeoutMs,
                                     gm::DownloadSink& sink) {
  gm::HttpResult res = emptyResult();
  esp_http_client_handle_t c = prepare(url, HTTP_METHOD_GET, timeoutMs, &res);
  if (c == nullptr) return res;
  esp_err_t err = esp_http_client_open(c, 0);
  const int64_t length = err == ESP_OK ? esp_http_client_fetch_headers(c) : -1;
  if (err != ESP_OK || length < 0) {
    snprintf(res.error, sizeof(res.error), "download: %s",
             esp_err_to_name(err));
    dropConnection();
    return res;
  }
  res.status = esp_http_client_get_status_code(c);
  res.transportOk = true;
  if (res.status != 200 || !sink.begin(static_cast<size_t>(length))) {
    dropConnection();
    return res;
  }
  static uint8_t buf[2048];
  while (true) {
    const int n =
        esp_http_client_read(c, reinterpret_cast<char*>(buf), sizeof(buf));
    if (n < 0) {
      res.transportOk = false;
      snprintf(res.error, sizeof(res.error), "connection lost during download");
      break;
    }
    if (n == 0) {
      if (!esp_http_client_is_complete_data_received(c)) {
        res.transportOk = false;
        snprintf(res.error, sizeof(res.error), "download incomplete");
      }
      break;
    }
    if (!sink.data(buf, static_cast<size_t>(n))) break;
  }
  dropConnection();  // large transfer: start the next request fresh
  return res;
}
