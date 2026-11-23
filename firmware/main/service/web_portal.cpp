// SPDX-License-Identifier: MIT
#include "service/web_portal.h"

#include <cstdlib>
#include <cstring>

#include "device.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* kTag = "portal";
static httpd_handle_t s_server = nullptr;
static volatile int64_t s_lastRequestUs = 0;

extern const uint8_t kIndexGzStart[] asm("_binary_index_html_gz_start");
extern const uint8_t kIndexGzEnd[] asm("_binary_index_html_gz_end");

static void touch() { s_lastRequestUs = esp_timer_get_time(); }

uint32_t webPortalIdleSeconds() {
  return static_cast<uint32_t>((esp_timer_get_time() - s_lastRequestUs) /
                               1000000);
}

static esp_err_t sendReply(httpd_req_t* req, const gm::PortalReply& r) {
  char status[16];
  snprintf(status, sizeof(status), "%d", r.status);
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, r.contentType);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  return httpd_resp_send(req, r.body, static_cast<ssize_t>(r.len));
}

static esp_err_t indexHandler(httpd_req_t* req) {
  touch();
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  return httpd_resp_send(req, reinterpret_cast<const char*>(kIndexGzStart),
                         kIndexGzEnd - kIndexGzStart);
}

static bool header(httpd_req_t* req, const char* name, char* out, size_t cap) {
  return httpd_req_get_hdr_value_str(req, name, out, cap) == ESP_OK;
}

static esp_err_t apiHandler(httpd_req_t* req) {
  touch();
  static char
      body[gm::kPortalMaxBody + 1];  // handlers run in the single httpd task
  int len = 0;
  if (req->content_len > gm::kPortalMaxBody) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    return ESP_FAIL;
  }
  while (len < static_cast<int>(req->content_len)) {
    const int n = httpd_req_recv(req, body + len, req->content_len - len);
    if (n <= 0) {
      return ESP_FAIL;  // timeout or closed
    }
    len += n;
  }
  body[len] = '\0';
  char pass[72] = "";
  const bool hasPass = header(req, "X-GM-Password", pass, sizeof(pass));
  gm::PortalRequest pr;
  pr.method = req->method == HTTP_POST ? "POST" : "GET";
  char path[64];
  snprintf(path, sizeof(path), "%.63s", req->uri);  // API paths are short
  char* q = std::strchr(path, '?');
  if (q != nullptr) *q = '\0';
  pr.path = path;
  pr.password = hasPass ? pass : nullptr;
  pr.body = body;
  pr.bodyLen = static_cast<size_t>(len);
  gm::PortalReply r = {};
  esp_err_t err;
  {
    DeviceLock lock;
    if (!device().portal->handle(pr, &r)) {
      r.status = 404;
      r.contentType = "application/json";
      r.body = "{\"ok\":false,\"error\":\"not found\"}";
      r.len = std::strlen(r.body);
    }
    err = sendReply(req, r);  // portal buffer valid while locked
  }
  if (r.restartAfter) {
    device().sys.restart = true;
  }
  return err;
}

static esp_err_t otaHandler(httpd_req_t* req) {
  touch();
  char pass[72] = "";
  char type[128] = "";
  char name[64] = "";
  header(req, "X-GM-Password", pass, sizeof(pass));
  header(req, "Content-Type", type, sizeof(type));
  header(req, "X-File-Name", name, sizeof(name));
  const bool multipart = std::strncmp(type, "multipart/form-data", 19) == 0;
  gm::PortalReply r = {};
  {
    DeviceLock lock;
    if (!device().portal->otaStart(
            pass, req->content_len,
            multipart ? gm::OtaBody::Multipart : gm::OtaBody::Raw, type, name,
            &r)) {
      return sendReply(req, r);
    }
  }
  static uint8_t buf[4096];
  size_t remaining = req->content_len;
  bool ok = true;
  while (remaining > 0 && ok) {
    const int n =
        httpd_req_recv(req, reinterpret_cast<char*>(buf),
                       remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;  // httpd recv timeout: keep waiting (connection still open)
    }
    if (n <= 0) {
      ok = false;
      break;
    }
    remaining -= static_cast<size_t>(n);
    touch();
    DeviceLock lock;
    ok = device().portal->otaFeed(buf, static_cast<size_t>(n));
  }
  esp_err_t err;
  {
    DeviceLock lock;
    if (!ok) {
      device().portal->otaAbort(remaining > 0 ? "connection lost during upload"
                                              : "upload rejected");
    }
    device().portal->otaFinish(&r);
    err = sendReply(req, r);
  }
  if (r.restartAfter) {
    device().sys.restart = true;
  }
  return err;
}

// Captive portal: any unknown URL (connectivity checks of Android/iOS/Windows)
// is redirected to the portal page.
static esp_err_t notFound(httpd_req_t* req, httpd_err_code_t) {
  if (std::strncmp(req->uri, "/api/", 5) == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"unknown endpoint\"}");
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  return httpd_resp_send(req, nullptr, 0);
}

esp_err_t webPortalStart() {
  if (s_server != nullptr) {
    return ESP_OK;
  }
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 8192;
  cfg.max_uri_handlers = 8;
  cfg.max_open_sockets = 5;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 10;
  cfg.send_wait_timeout = 10;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  esp_err_t err = httpd_start(&s_server, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "httpd_start: %s", esp_err_to_name(err));
    return err;
  }
  const httpd_uri_t uris[] = {
      {"/", HTTP_GET, indexHandler, nullptr},
      {"/index.html", HTTP_GET, indexHandler, nullptr},
      {"/api/ota", HTTP_POST, otaHandler, nullptr},
      {"/api/*", HTTP_GET, apiHandler, nullptr},
      {"/api/*", HTTP_POST, apiHandler, nullptr},
  };
  for (const httpd_uri_t& u : uris) {
    httpd_register_uri_handler(s_server, &u);
  }
  httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, notFound);
  touch();
  ESP_LOGI(kTag, "portal at http://192.168.4.1/");
  return ESP_OK;
}

void webPortalStop() {
  if (s_server != nullptr) {
    httpd_stop(s_server);
    s_server = nullptr;
  }
}
