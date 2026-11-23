// SPDX-License-Identifier: MIT
#ifndef GM_PC_WEB_SERVER_H
#define GM_PC_WEB_SERVER_H

#include <memory>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace gmpc {

class PcDevice;

// Local service portal for the PC simulator (cpp-httplib). Serves the shared
// portal/ files and forwards /api/* to gm::Portal through PcDevice.
class WebServer {
 public:
  WebServer(PcDevice& dev, const std::string& portalDir);
  ~WebServer();

  // port 0 = pick a free port. Returns the bound port or -1.
  int start(const std::string& host, int port);
  void stop();

 private:
  void setup();

  PcDevice& dev_;
  std::string portalDir_;
  std::unique_ptr<httplib::Server> svr_;
  std::thread thread_;
};

}  // namespace gmpc

#endif
