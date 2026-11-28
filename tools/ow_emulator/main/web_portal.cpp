// SPDX-License-Identifier: MIT
// Standalone adaptation of firmware/main/service/web_portal.cpp.
#include "web_portal.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "config_store.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "slave_bus.h"

namespace {
httpd_handle_t s_server = nullptr;
ow::Config s_config;
extern const uint8_t kIndexStart[] asm("_binary_index_html_start");
extern const uint8_t kIndexEnd[] asm("_binary_index_html_end");

void headers(httpd_req_t* req, const char* type) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

esp_err_t indexHandler(httpd_req_t* req) {
  headers(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  return httpd_resp_send(req, reinterpret_cast<const char*>(kIndexStart),
                         kIndexEnd - kIndexStart);
}

esp_err_t configGet(httpd_req_t* req) {
  char* json = configJson(s_config);
  if (!json)
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "out of memory");
  headers(req, "application/json");
  const esp_err_t err = httpd_resp_sendstr(req, json);
  cJSON_free(json);
  return err;
}

esp_err_t configPost(httpd_req_t* req) {
  static char body[kMaxConfigJson + 1];
  if (req->content_len == 0 || req->content_len > kMaxConfigJson)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "body must contain 1..12288 bytes");
  size_t length = 0;
  while (length < req->content_len) {
    const int n = httpd_req_recv(req, body + length, req->content_len - length);
    if (n <= 0) {
      ESP_LOGW("portal", "incomplete config body: %d", n);
      return ESP_FAIL;
    }
    length += n;
  }
  body[length] = '\0';
  if (std::memchr(body, '\0', length))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "embedded NUL");
  ow::Config next;
  char error[128];
  if (!configParse(body, &next, error, sizeof(error)))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
  const esp_err_t err = configSave(next);
  if (err != ESP_OK) {
    ESP_LOGE("portal", "NVS save: %s", esp_err_to_name(err));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "NVS save failed; config not applied");
  }
  s_config = next;
  slaveBusQueueConfig(next);
  headers(req, "application/json");
  return httpd_resp_sendstr(
      req, "{\"ok\":true,\"apply\":\"next_reset\",\"saved_to_nvs\":true}");
}

esp_err_t logHandler(httpd_req_t* req) {
  static ow::Transaction logs[ow::kLogSize];
  const size_t count = slaveBusLogs(logs, ow::kLogSize);
  const BusStats stats = slaveBusStats();
  char line[384];
  headers(req, "text/plain; charset=utf-8");
  int16_t realTemperature = 0;
  const bool realValid = slaveBusRealReading(&realTemperature);
  std::snprintf(line, sizeof(line),
                "resets=%" PRIu32 " invalid_commands=%" PRIu32
                " unmatched_roms=%" PRIu32 " short_pulses=%" PRIu32
                " late_reactions=%" PRIu32 " max_reaction_cycles=%" PRIu32
                " first_edges=%" PRIu32 " overruns=%" PRIu32
                " stuck_low=%" PRIu32 " real_errors=%" PRIu32
                " real_valid=%d real_temp=%.4f pending_config=%d"
                " sequence=%" PRIu32 "\n",
                stats.protocol.resets, stats.protocol.invalidCommands,
                stats.protocol.unmatchedRoms, stats.protocol.shortPulses,
                stats.lateReactions, stats.maxReactionCycles, stats.firstEdges,
                stats.overruns, stats.stuckLow, stats.realErrors, realValid,
                realTemperature / 16.0, stats.pendingConfig, stats.logSequence);
  esp_err_t err = httpd_resp_sendstr_chunk(req, line);
  if (err != ESP_OK) return err;
  for (size_t i = 0; i < count; ++i) {
    const auto& log = logs[i];
    std::snprintf(line, sizeof(line),
                  "t_us=%" PRIu64
                  " ROM_CMD=%02X ROM=%02X%02X%02X%02X%02X%02X%02X%02X"
                  " FUNC=%02X selected=%02X bytes=%u bits=%u invalid=%d\n",
                  log.timeUs, log.romCommand, log.rom[0], log.rom[1],
                  log.rom[2], log.rom[3], log.rom[4], log.rom[5], log.rom[6],
                  log.rom[7], log.function, log.selected, log.readBits / 8,
                  log.readBits, log.invalidCommand);
    err = httpd_resp_sendstr_chunk(req, line);
    if (err != ESP_OK) return err;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t redirect(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, nullptr, 0);
}

esp_err_t notFound(httpd_req_t* req, httpd_err_code_t) { return redirect(req); }
}  // namespace

esp_err_t webPortalStart(const ow::Config& config) {
  s_config = config;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 8192;
  cfg.max_uri_handlers = 10;
  cfg.max_open_sockets = 5;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 5;
  cfg.send_wait_timeout = 5;
  esp_err_t err = httpd_start(&s_server, &cfg);
  if (err != ESP_OK) return err;
  const httpd_uri_t uris[] = {
      {.uri = "/",
       .method = HTTP_GET,
       .handler = indexHandler,
       .user_ctx = nullptr},
      {.uri = "/index.html",
       .method = HTTP_GET,
       .handler = indexHandler,
       .user_ctx = nullptr},
      {.uri = "/config.json",
       .method = HTTP_GET,
       .handler = configGet,
       .user_ctx = nullptr},
      {.uri = "/config.json",
       .method = HTTP_POST,
       .handler = configPost,
       .user_ctx = nullptr},
      {.uri = "/log.txt",
       .method = HTTP_GET,
       .handler = logHandler,
       .user_ctx = nullptr},
      {.uri = "/generate_204",
       .method = HTTP_GET,
       .handler = redirect,
       .user_ctx = nullptr},
      {.uri = "/hotspot-detect.html",
       .method = HTTP_GET,
       .handler = redirect,
       .user_ctx = nullptr},
      {.uri = "/ncsi.txt",
       .method = HTTP_GET,
       .handler = redirect,
       .user_ctx = nullptr},
  };
  for (const auto& uri : uris) {
    err = httpd_register_uri_handler(s_server, &uri);
    if (err != ESP_OK) return err;
  }
  return httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, notFound);
}
