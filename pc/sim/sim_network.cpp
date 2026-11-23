// SPDX-License-Identifier: MIT
#include "sim/sim_network.h"

#include <httplib.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gmpc {

bool SimNetwork::connect(const gm::Config& cfg, uint32_t timeoutMs, char* err,
                         size_t errCap) {
  ++connects_;
  if (cfg.wifiSsid[0] == '\0') {
    std::snprintf(err, errCap, "Wi-Fi SSID not configured");
    return false;
  }
  if (!up_) {
    clock_.sleepMs(timeoutMs);  // a real attempt burns the whole timeout
    std::snprintf(err, errCap, "AP '%.32s' not found (simulated outage)",
                  cfg.wifiSsid);
    return false;
  }
  clock_.sleepMs(1500);  // association + DHCP
  connected_ = true;
  return true;
}

bool SimNetwork::syncTime(uint32_t timeoutMs) {
  if (!connected_) {
    return false;
  }
  (void)timeoutMs;
  ++syncs_;
  clock_.sleepMs(300);
  clock_.setSynced(true);
  return true;
}

const char* SimNetwork::stateText() {
  if (!up_) {
    return connected_ ? "connected" : "off (outage simulated)";
  }
  return connected_ ? "connected" : "off";
}

std::string PcHttpClient::lastUrl() { return lastUrl_; }

gm::HttpResult PcHttpClient::postJson(const char* url, const char* body,
                                      size_t len, uint32_t timeoutMs,
                                      char* resp, size_t respCap) {
  gm::HttpResult res;
  res.transportOk = false;
  res.status = -1;
  res.error[0] = '\0';
  ++posts_;
  lastUrl_ = url;
  std::string reply;
  int status = -1;

  if (std::strncmp(url, "mock://", 7) == 0) {
    const char* path = std::strchr(url + 7, '/');
    status = mock_.handle(path != nullptr ? path : "/", std::string(body, len),
                          &reply);
  } else if (std::strncmp(url, "http://", 7) == 0) {
    const char* hostStart = url + 7;
    const char* path = std::strchr(hostStart, '/');
    const std::string origin(url, path != nullptr
                                      ? static_cast<size_t>(path - url)
                                      : std::strlen(url));
    httplib::Client cli(origin);
    const time_t secs = static_cast<time_t>(timeoutMs / 1000u);
    cli.set_connection_timeout(secs, 0);
    cli.set_read_timeout(secs, 0);
    cli.set_write_timeout(secs, 0);
    auto r =
        cli.Post(path != nullptr ? path : "/", body, len, "application/json");
    if (!r) {
      std::snprintf(res.error, sizeof(res.error), "%s",
                    httplib::to_string(r.error()).c_str());
      return res;
    }
    status = r->status;
    reply = r->body;
  } else {
    std::snprintf(res.error, sizeof(res.error),
                  "URL scheme not supported on PC (use mock:// or http://)");
    return res;
  }

  if (lostResponse_) {
    std::snprintf(res.error, sizeof(res.error),
                  "connection reset before response (simulated)");
    return res;  // the server DID store the data
  }
  res.transportOk = true;
  res.status = status;
  if (respCap > 0) {
    std::snprintf(resp, respCap, "%s", reply.c_str());
  }
  return res;
}

static gm::HttpResult emptyResult() {
  gm::HttpResult res;
  res.transportOk = false;
  res.status = -1;
  res.error[0] = '\0';
  return res;
}

bool PcHttpClient::readUpdateFile(const char* url, std::string* out,
                                  gm::HttpResult* res) {
  const char* name = url + std::strlen("mock://updates/");
  if (std::strstr(name, "..") != nullptr || name[0] == '\0') {
    res->transportOk = true;
    res->status = 400;
    return false;
  }
  std::ifstream in(updatesDir_ + "/" + name, std::ios::binary);
  res->transportOk = true;
  if (!in) {
    res->status = 404;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  res->status = 200;
  return true;
}

static void splitUrl(const char* url, std::string* origin, std::string* path) {
  const char* p = std::strchr(url + 7, '/');
  *origin = std::string(
      url, p != nullptr ? static_cast<size_t>(p - url) : std::strlen(url));
  *path = p != nullptr ? p : "/";
}

gm::HttpResult PcHttpClient::get(const char* url, uint32_t timeoutMs,
                                 char* resp, size_t respCap) {
  gm::HttpResult res = emptyResult();
  std::string body;
  if (std::strncmp(url, "mock://updates/", 15) == 0) {
    readUpdateFile(url, &body, &res);
  } else if (std::strncmp(url, "http://", 7) == 0) {
    std::string origin, path;
    splitUrl(url, &origin, &path);
    httplib::Client cli(origin);
    const time_t secs = static_cast<time_t>(timeoutMs / 1000u);
    cli.set_connection_timeout(secs, 0);
    cli.set_read_timeout(secs, 0);
    cli.set_follow_location(true);  // release downloads redirect
    auto r = cli.Get(path);
    if (!r) {
      std::snprintf(res.error, sizeof(res.error), "%s",
                    httplib::to_string(r.error()).c_str());
      return res;
    }
    res.transportOk = true;
    res.status = r->status;
    body = r->body;
  } else {
    std::snprintf(res.error, sizeof(res.error),
                  "URL scheme not supported on PC (use mock://updates/ or "
                  "http://)");
    return res;
  }
  if (respCap > 0) {
    const size_t n = body.size() < respCap - 1 ? body.size() : respCap - 1;
    std::memcpy(resp, body.data(), n);
    resp[n] = '\0';
  }
  return res;
}

gm::HttpResult PcHttpClient::download(const char* url, uint32_t timeoutMs,
                                      gm::DownloadSink& sink) {
  gm::HttpResult res = emptyResult();
  if (std::strncmp(url, "mock://updates/", 15) == 0) {
    std::string body;
    if (!readUpdateFile(url, &body, &res)) {
      return res;
    }
    if (!sink.begin(body.size())) {
      return res;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(body.data());
    for (size_t off = 0; off < body.size(); off += 4096) {
      const size_t n = body.size() - off < 4096 ? body.size() - off : 4096;
      if (!sink.data(p + off, n)) {
        break;
      }
    }
    return res;
  }
  if (std::strncmp(url, "http://", 7) != 0) {
    std::snprintf(res.error, sizeof(res.error), "URL scheme not supported");
    return res;
  }
  std::string origin, path;
  splitUrl(url, &origin, &path);
  httplib::Client cli(origin);
  const time_t secs = static_cast<time_t>(timeoutMs / 1000u);
  cli.set_connection_timeout(secs, 0);
  cli.set_read_timeout(secs, 0);
  cli.set_follow_location(true);
  int httpStatus = -1;
  bool begun = false;
  auto r = cli.Get(
      path,
      [&](const httplib::Response& response) {
        httpStatus = response.status;
        if (response.status >= 300 && response.status < 400) {
          return true;  // redirect hop: httplib follows it
        }
        if (response.status != 200) {
          return false;  // nothing reaches the sink
        }
        const std::string len = response.get_header_value("Content-Length");
        begun = sink.begin(
            static_cast<size_t>(std::strtoull(len.c_str(), nullptr, 10)));
        return begun;
      },
      [&](const char* data, size_t n) {
        // bodies of redirect responses are ignored
        return !begun || sink.data(reinterpret_cast<const uint8_t*>(data), n);
      });
  if (!r) {
    if (httpStatus > 0) {  // stopped by us (HTTP error or sink refused)
      res.transportOk = true;
      res.status = httpStatus;
      return res;
    }
    std::snprintf(res.error, sizeof(res.error), "%s",
                  httplib::to_string(r.error()).c_str());
    return res;
  }
  res.transportOk = true;
  res.status = r->status;
  return res;
}

}  // namespace gmpc
