// SPDX-License-Identifier: MIT
#include "platform/web_server.h"

#include <httplib.h>

#include <cstdlib>

#include "app/pc_device.h"
#include "util/log.h"

namespace gmpc {

static const char* kTag = "web";

WebServer::WebServer(PcDevice& dev, const std::string& portalDir)
    : dev_(dev), portalDir_(portalDir), svr_(new httplib::Server()) {
  setup();
}

WebServer::~WebServer() { stop(); }

static void sendReply(httplib::Response& res, const gm::PortalReply& r,
                      const std::string& body) {
  res.status = r.status;
  res.set_header("Cache-Control", "no-store");
  res.set_content(body, r.contentType);
}

void WebServer::setup() {
  httplib::Server& s = *svr_;
  s.set_payload_max_length(4u * 1024u * 1024u);
  s.set_default_headers(
      {{"X-Content-Type-Options", "nosniff"}, {"X-Frame-Options", "DENY"}});

  // Streaming OTA upload: a browser file form (multipart, parsed here by
  // cpp-httplib) or the raw image as body.
  s.Post("/api/ota", [this](const httplib::Request& req, httplib::Response& res,
                            const httplib::ContentReader& reader) {
    const std::string pass = req.get_header_value("X-GM-Password");
    const size_t len = static_cast<size_t>(std::strtoull(
        req.get_header_value("Content-Length").c_str(), nullptr, 10));
    const bool multipart = req.is_multipart_form_data();
    gm::PortalReply r;
    std::string body;
    bool started = false;
    bool ok = true;
    if (!multipart) {
      const std::string name = req.get_header_value("X-File-Name");
      started = dev_.otaStart(pass.c_str(), len, gm::OtaBody::Raw, name.c_str(),
                              &r, &body);
      if (started) {
        ok = reader([&](const char* data, size_t n) {
          return dev_.otaFeed(reinterpret_cast<const uint8_t*>(data), n);
        });
      }
    } else {
      bool inFile = false;
      bool fileSeen = false;
      ok = reader(
          [&](const httplib::FormData& part) {
            inFile = !fileSeen && !part.filename.empty();
            if (inFile) {
              fileSeen = true;
              started =
                  dev_.otaStart(pass.c_str(), len, gm::OtaBody::DecodedPart,
                                part.filename.c_str(), &r, &body);
              return started;  // refused: stop reading
            }
            return true;
          },
          [&](const char* data, size_t n) {
            return !inFile ||
                   dev_.otaFeed(reinterpret_cast<const uint8_t*>(data), n);
          });
      if (!fileSeen && body.empty()) {
        res.status = 400;
        res.set_content("{\"ok\":false,\"error\":\"no file in form\"}",
                        "application/json");
        return;
      }
    }
    if (!started) {
      sendReply(res, r, body);
      return;
    }
    if (!ok) {
      GM_LOGW(kTag, "OTA upload interrupted or malformed");
      dev_.otaAbort("upload interrupted or malformed");
    }
    dev_.otaFinish(&r, &body);
    sendReply(res, r, body);
  });

  auto api = [this](const httplib::Request& req, httplib::Response& res) {
    gm::PortalRequest pr;
    const std::string pass = req.get_header_value("X-GM-Password");
    pr.method = req.method.c_str();
    pr.path = req.path.c_str();
    pr.password = req.has_header("X-GM-Password") ? pass.c_str() : nullptr;
    pr.body = req.body.c_str();
    pr.bodyLen = req.body.size();
    gm::PortalReply r;
    std::string body;
    if (dev_.handle(pr, &r, &body)) {
      sendReply(res, r, body);
    } else {
      res.status = 404;
    }
  };
  s.Get(R"(/api/.*)", api);
  s.Post(R"(/api/.*)", api);

  if (!s.set_mount_point("/", portalDir_)) {
    GM_LOGE(kTag, "portal directory not found: %s", portalDir_.c_str());
  }
}

int WebServer::start(const std::string& host, int port) {
  int bound = port;
  if (port == 0) {
    bound = svr_->bind_to_any_port(host);
  } else if (!svr_->bind_to_port(host, port)) {
    bound = -1;
  }
  if (bound <= 0) {
    return -1;
  }
  thread_ = std::thread([this]() { svr_->listen_after_bind(); });
  svr_->wait_until_ready();
  return bound;
}

void WebServer::stop() {
  if (svr_) {
    svr_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

}  // namespace gmpc
