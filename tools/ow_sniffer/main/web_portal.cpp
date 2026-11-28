// SPDX-License-Identifier: MIT
#include <inttypes.h>

#include <cstdio>
#include <cstring>

#include "capture.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ow_decoder.h"
#include "services.h"

namespace ow {
namespace {
constexpr size_t kRecent = 20;
// HTTP handlers execute serially in the single server task.
char recent[kRecent][1024];
const char page[] = R"html(<!doctype html>
<html lang="pl"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OW Sniffer</title>
<style>body{font:16px system-ui;margin:1em;background:#111;color:#eee}
button,a{margin:.5em;color:#6cf}button{background:#222;padding:.7em}
pre{white-space:pre-wrap;overflow-wrap:anywhere;font-size:12px}</style>
<h1>OW Sniffer</h1><p id="status">Ladowanie...</p>
<button id="clear">Wyczysc bufor</button>
<a href="/frames.txt" download>frames.txt</a><a href="/raw.csv" download>raw.csv</a>
<p id="error"></p><pre id="frames"></pre>
<script>
async function update(){
 try{
  const sr=await fetch('/status.json',{cache:'no-store'});
  if(!sr.ok)throw Error('Status HTTP '+sr.status);
  const s=await sr.json();
  document.getElementById('status').textContent=
   'Uptime: '+s.uptime_s.toFixed(1)+' s | Rekordy: '+s.records+
   ' | Nadpisania: '+s.overwritten+' | Anomalie ISR: '+s.anomalies+
   ' | DATA: '+s.line+' | Nagrywanie: '+s.recording_s.toFixed(1)+' s';
  const fr=await fetch('/frames.txt?recent=1',{cache:'no-store'});
  if(!fr.ok)throw Error('Ramki HTTP '+fr.status);
  document.getElementById('frames').textContent=await fr.text();
  document.getElementById('error').textContent='';
 }catch(e){document.getElementById('error').textContent=e.message}
 finally{setTimeout(update,2000)}
}
document.getElementById('clear').onclick=async()=>{
 if(!confirm('Wyczysc nagranie?'))return;
 try{
  const r=await fetch('/clear',{method:'POST'});
  if(!r.ok)throw Error('Clear HTTP '+r.status);
 }catch(e){document.getElementById('error').textContent=e.message}
};
update();
</script></html>)html";

void headers(httpd_req_t* req, const char* type) {
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

esp_err_t indexHandler(httpd_req_t* req) {
  headers(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, page, sizeof(page) - 1);
}

esp_err_t statusHandler(httpd_req_t* req) {
  const Snapshot s = captureSnapshot();
  char body[320];
  std::snprintf(body, sizeof(body),
                "{\"uptime_s\":%.3f,\"recording_s\":%.3f,\"records\":%" PRIu64
                ",\"overwritten\":%" PRIu64 ",\"anomalies\":%" PRIu64
                ",\"line\":%u,\"capacity\":%zu}",
                s.uptime_us / 1000000.0, s.recording_us / 1000000.0,
                s.end - s.begin, s.overwritten, s.anomalies, s.tail.level,
                kCapacity);
  headers(req, "application/json");
  return httpd_resp_sendstr(req, body);
}

esp_err_t clearHandler(httpd_req_t* req) {
  if (req->content_len != 0)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "body must be empty");
  captureClear();
  headers(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

struct Stream {
  httpd_req_t* req;
  char buffer[2048];
  size_t used = 0;
  esp_err_t error = ESP_OK;
  bool flush() {
    if (error != ESP_OK) return false;
    if (used) {
      error = httpd_resp_send_chunk(req, buffer, used);
      used = 0;
    }
    return error == ESP_OK;
  }
  bool append(const char* text) {
    const size_t n = std::strlen(text);
    if (used + n > sizeof(buffer) && !flush()) return false;
    if (n > sizeof(buffer)) {
      error = ESP_ERR_INVALID_SIZE;
      ESP_LOGE("portal", "output line exceeds stream buffer");
      return false;
    }
    std::memcpy(buffer + used, text, n);
    used += n;
    return true;
  }
};

bool streamSink(void* context, const char* line) {
  return static_cast<Stream*>(context)->append(line);
}

struct Recent {
  size_t count = 0;
};
bool recentSink(void* context, const char* line) {
  auto* r = static_cast<Recent*>(context);
  std::snprintf(recent[r->count % kRecent], sizeof(recent[0]), "%s", line);
  ++r->count;
  return true;
}

esp_err_t changed(Stream& stream) {
  ESP_LOGW("portal", "export invalidated by overwrite/clear; retry download");
  if (!stream.append(
          "# ERROR: snapshot overwritten or cleared; retry download\n") ||
      !stream.flush())
    return stream.error;
  return httpd_resp_send_chunk(stream.req, nullptr, 0);
}

esp_err_t framesHandler(httpd_req_t* req) {
  headers(req, "text/plain; charset=utf-8");
  char query[32]{};
  const bool last =
      httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      std::strcmp(query, "recent=1") == 0;
  const Snapshot s = captureSnapshot();
  Stream stream{req, {}};
  Recent r;
  Decoder decoder(last ? recentSink : streamSink,
                  last ? static_cast<void*>(&r) : static_cast<void*>(&stream),
                  static_cast<double>(s.offset_us));
  RawRecord raw;
  for (uint64_t i = s.begin; i < s.end; ++i) {
    if (!captureRead(s, i, &raw)) return changed(stream);
    if (!decoder.feed({raw.level, static_cast<double>(raw.duration_us)}))
      return stream.error != ESP_OK ? stream.error : ESP_FAIL;
  }
  if (s.tail.duration_us &&
      !decoder.feed({s.tail.level, static_cast<double>(s.tail.duration_us)}))
    return stream.error != ESP_OK ? stream.error : ESP_FAIL;
  if (!decoder.finish())
    return stream.error != ESP_OK ? stream.error : ESP_FAIL;
  if (!captureValid(s)) return changed(stream);
  if (last) {
    const size_t begin = r.count > kRecent ? r.count - kRecent : 0;
    for (size_t i = begin; i < r.count; ++i)
      if (!stream.append(recent[i % kRecent])) return stream.error;
  }
  if (!stream.flush()) return stream.error;
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t rawHandler(httpd_req_t* req) {
  headers(req, "text/csv; charset=utf-8");
  const Snapshot s = captureSnapshot();
  // A raw export begins at the oldest retained state; preserve its origin.
  char offset_header[96];
  std::snprintf(offset_header, sizeof(offset_header),
                "# recording_offset_us=%" PRIu64 "\n", s.offset_us);
  Stream stream{req, {}};
  stream.append("# level,duration_us\n");
  stream.append(offset_header);
  for (uint64_t i = s.begin; i <= s.end; ++i) {
    RawRecord raw = s.tail;
    if (i < s.end && !captureRead(s, i, &raw)) return changed(stream);
    if (!raw.duration_us) continue;
    char row[48];
    std::snprintf(row, sizeof(row), "%u,%" PRIu64 "\n", raw.level,
                  raw.duration_us);
    if (!stream.append(row)) return stream.error;
  }
  if (!captureValid(s)) return changed(stream);
  if (!stream.flush()) return stream.error;
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// Local equivalent of firmware/main/service/web_portal.cpp's captive redirect.
esp_err_t notFound(httpd_req_t* req, httpd_err_code_t) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, nullptr, 0);
}
}  // namespace

esp_err_t webStart() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.max_uri_handlers = 5;
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;
  config.send_wait_timeout = 5;
  config.recv_wait_timeout = 5;
  httpd_handle_t server = nullptr;
  ESP_RETURN_ON_ERROR(httpd_start(&server, &config), "portal", "httpd_start");
  const auto route = [](const char* uri, httpd_method_t method,
                        esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t result{};
    result.uri = uri;
    result.method = method;
    result.handler = handler;
    return result;
  };
  const httpd_uri_t routes[] = {route("/", HTTP_GET, indexHandler),
                                route("/status.json", HTTP_GET, statusHandler),
                                route("/frames.txt", HTTP_GET, framesHandler),
                                route("/raw.csv", HTTP_GET, rawHandler),
                                route("/clear", HTTP_POST, clearHandler)};
  for (const auto& entry : routes)
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &entry), "portal",
                        "register route");
  return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, notFound);
}
}  // namespace ow
