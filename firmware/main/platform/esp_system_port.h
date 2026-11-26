// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>

#include "app/platform.h"

class WifiLink;

class EspSystemPort : public gm::SystemPort {
 public:
  void init(WifiLink* wifi);

  const char* platformName() override { return "esp32c6"; }
  int batteryMv() override;
  const char* wakeReason() override { return wake_; }
  const char* resetReason() override { return reset_; }
  void requestRestart() override { restart = true; }
  void writePlatformJson(gm::JsonWriter& w, bool detailed) override;
  bool platformAction(const char* action, const gm::Form& args, char* msg,
                      size_t cap) override;
  const char* rejectedFirmware() override { return rejected_; }
  void configApplied(const gm::Config& cfg) override;
  bool boardTempCenti(int16_t* out) override;

  // Shared with the main loop.
  std::atomic<bool> restart{false};
  std::atomic<bool> enterService{false};
  std::atomic<bool> exitService{false};
  std::atomic<bool> serviceMode{false};
  std::atomic<uint32_t> serviceIdleS{0};
  // Service window length requested remotely (0 = default idle timeout).
  std::atomic<uint32_t> serviceMinutes{0};
  bool wokeByButton = false;
  char apSsid[33] = "";
  // Cached from the configuration for the Wi-Fi drop hook (no lock there).
  std::atomic<bool> lanWake{true};
  char deviceName[32] = "";
  // Copy of deviceName that is safe against a concurrent configApplied().
  void copyDeviceName(char* out, size_t cap);
  bool coldBoot = true;

 private:
  WifiLink* wifi_ = nullptr;
  const char* wake_ = "power-on";
  const char* reset_ = "unknown";
  int batteryMv_ = -1;
  bool batteryRead_ = false;
  char rejected_[33] = "";
  uint32_t bootCount_ = 0;
  bool crashCleared_ = false;
  void writeDetails(gm::JsonWriter& w);
  void writeCrash(gm::JsonWriter& w);
};
