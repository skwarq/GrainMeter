// SPDX-License-Identifier: MIT
#ifndef GM_TEST_DEVICE_H
#define GM_TEST_DEVICE_H

#include <filesystem>
#include <memory>
#include <random>
#include <string>

#include "app/pc_device.h"

// Simulated device with a manual clock in a fresh temporary directory.
struct TestDevice {
  std::string dir;
  std::unique_ptr<gmpc::PcDevice> dev;

  explicit TestDevice(const std::string& name, int sensors = 8) {
    // Unique per process and instance: ctest runs test processes in parallel.
    std::random_device rd;
    dir = (std::filesystem::temp_directory_path() /
           ("gm_test_" + name + "_" + std::to_string(rd())))
              .string();
    std::filesystem::remove_all(dir);
    gmpc::PcDevice::Options o;
    o.dataDir = dir;
    o.manualClock = true;
    o.sensors = sensors;
    o.servicePassword = "test-password";
    dev.reset(new gmpc::PcDevice(o));
    std::string err;
    started = dev->start(&err);
  }
  ~TestDevice() {
    dev.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  // Advances simulated time in steps and runs the scheduler like the
  // firmware's wake cycle.
  void runFor(uint32_t seconds, uint32_t step = 60) {
    for (uint32_t t = 0; t < seconds; t += step) {
      dev->tick();
      dev->clock().advanceMs(static_cast<uint64_t>(step) * 1000u);
    }
    dev->tick();
  }
  bool started = false;
};

#endif
