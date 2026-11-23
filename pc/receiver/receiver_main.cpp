// SPDX-License-Identifier: MIT
// gm_receiver: local ThingSpeak-compatible receiver. Point the simulator's
// report_url at http://127.0.0.1:8090 to exercise the real HTTP path without
// sending anything to a ThingSpeak account.
#include <httplib.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "sim/mock_thingspeak.h"

namespace {
httplib::Server* g_server = nullptr;
void onSignal(int) {
  if (g_server != nullptr) g_server->stop();
}
}  // namespace

int main(int argc, char** argv) {
  int port = 8090;
  std::string log = "receiver.jsonl";
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--port") {
      port = std::atoi(argv[i + 1]);
    } else if (a == "--log") {
      log = argv[i + 1];
    }
  }
  gmpc::MockThingSpeak mock(log);
  httplib::Server svr;
  g_server = &svr;
  svr.Post(R"(/channels/\d+/bulk_update\.json)",
           [&mock](const httplib::Request& req, httplib::Response& res) {
             std::string reply;
             res.status = mock.handle(req.path, req.body, &reply);
             res.set_content(reply, "application/json");
             std::printf("POST %s -> %d\n", req.path.c_str(), res.status);
             std::fflush(stdout);
           });
  svr.Get(R"(/channels/(\d+)/feeds\.json)",
          [&mock](const httplib::Request& req, httplib::Response& res) {
            const uint32_t ch = static_cast<uint32_t>(
                std::strtoul(req.matches[1].str().c_str(), nullptr, 10));
            std::string out = "{\"feeds\":[";
            bool first = true;
            for (const auto& e : mock.entries(ch)) {
              out += first ? "{" : ",{";
              first = false;
              out += "\"created_at\":\"" + e.createdAt + "\"";
              for (const auto& kv : e.values) {
                const bool str = kv.first == "status";
                out += ",\"" + kv.first +
                       "\":" + (str ? "\"" + kv.second + "\"" : kv.second);
              }
              out += "}";
            }
            out += "]}";
            res.set_content(out, "application/json");
          });
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::printf("gm_receiver listening on http://127.0.0.1:%d (log %s)\n", port,
              log.c_str());
  std::fflush(stdout);
  return svr.listen("127.0.0.1", port) ? 0 : 1;
}
