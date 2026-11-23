// SPDX-License-Identifier: MIT
// GrainMeter PC simulator: shared application logic with a simulated probe,
// simulated Wi-Fi and a local ThingSpeak stand-in, plus the service portal.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "app/pc_device.h"
#include "platform/web_server.h"
#include "util/log.h"

#ifndef GM_PORTAL_DIR
#define GM_PORTAL_DIR "portal"
#endif

namespace {

std::atomic<bool> g_running(true);

void onSignal(int) { g_running = false; }

void logSink(gm::LogLevel level, const char* tag, const char* message) {
  static const char kLevels[] = {'E', 'W', 'I', 'D'};
  std::fprintf(stderr, "%c [%-6s] %s\n", kLevels[static_cast<int>(level)], tag,
               message);
}

void usage() {
  std::printf(
      "Usage: grainmeter_pc [options]\n"
      "  --port N           portal port (default 8080, 0 = any)\n"
      "  --bind ADDR        bind address (default 127.0.0.1)\n"
      "  --data-dir DIR     config, reports and OTA files (default ./pc-data)\n"
      "  --config FILE      configuration file (default "
      "<data-dir>/grainmeter-pc.conf)\n"
      "  --speed X          simulated seconds per real second (default 60)\n"
      "  --sensors N        simulated sensors on the probe (default 8)\n"
      "  --portal-dir DIR   portal files (default: source tree)\n"
      "  --password P       set the service password\n"
      "  --no-console       do not read commands from stdin\n"
      "  --no-time          start without wall clock (like a fresh ESP32)\n"
      "  --run-for S        exit after S real seconds (smoke tests)\n");
}

}  // namespace

int main(int argc, char** argv) {
  gm::setLogSink(logSink);
  gmpc::PcDevice::Options opt;
  std::string host = "127.0.0.1";
  std::string portalDir = GM_PORTAL_DIR;
  int port = 8080;
  bool console = true;
  double runFor = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool hasValue = i + 1 < argc;
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a == "--port" && hasValue) {
      port = std::atoi(argv[++i]);
    } else if (a == "--bind" && hasValue) {
      host = argv[++i];
    } else if (a == "--data-dir" && hasValue) {
      opt.dataDir = argv[++i];
    } else if (a == "--config" && hasValue) {
      opt.configPath = argv[++i];
    } else if (a == "--speed" && hasValue) {
      opt.speed = std::atof(argv[++i]);
    } else if (a == "--sensors" && hasValue) {
      opt.sensors = std::atoi(argv[++i]);
    } else if (a == "--portal-dir" && hasValue) {
      portalDir = argv[++i];
    } else if (a == "--password" && hasValue) {
      opt.servicePassword = argv[++i];
    } else if (a == "--no-time") {
      opt.wallClockAtStart = false;
    } else if (a == "--no-console") {
      console = false;
    } else if (a == "--run-for" && hasValue) {
      runFor = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  gmpc::PcDevice dev(opt);
  std::string err;
  if (!dev.start(&err)) {
    std::fprintf(stderr, "start failed: %s\n", err.c_str());
    return 1;
  }
  gmpc::WebServer web(dev, portalDir);
  const int bound = web.start(host, port);
  if (bound < 0) {
    std::fprintf(stderr, "cannot listen on %s:%d\n", host.c_str(), port);
    return 1;
  }
  std::printf("GrainMeter PC simulator %s\n", GM_FW_VERSION);
  std::printf("  portal:        http://%s:%d/\n", host.c_str(), bound);
  std::printf("  config file:   %s (service password = svc_pass)\n",
              dev.configPath().c_str());
  std::printf("  reports log:   %s/reports.jsonl\n", opt.dataDir.c_str());
  std::printf("  time speed:    x%g\n", opt.speed);
  std::printf("Type 'help' for console commands, Ctrl+C to quit.\n");
  std::fflush(stdout);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  std::thread consoleThread;
  if (console) {
    consoleThread = std::thread([&dev]() {
      std::cout << "> " << std::flush;
      std::string line;
      while (g_running && std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") {
          g_running = false;
          break;
        }
        bool restart = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Same prompt as the ESP32 USB console (scripts/provision.py).
        std::cout << dev.shell(line, &restart) << "> " << std::flush;
        if (restart) {
          dev.requestRestart();
        }
      }
    });
    consoleThread.detach();  // blocked in getline; process exit ends it
  }

  const auto start = std::chrono::steady_clock::now();
  while (g_running) {
    dev.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (runFor > 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                .count() >= runFor) {
      break;
    }
  }
  web.stop();
  std::printf("bye\n");
  return 0;
}
