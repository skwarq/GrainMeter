// SPDX-License-Identifier: MIT
#pragma once

#include "driver/gpio.h"
#include "onewire/onewire.h"

// 1-Wire master on one open-drain GPIO (standard speed). Only the time
// slots live here; search/CRC/DS18B20 logic is shared with the PC build.
class OneWireGpio : public gm::OneWireBus {
 public:
  explicit OneWireGpio(gpio_num_t pin) : pin_(pin) {}
  esp_err_t init();
  // True when the idle line reads high (pull-up present, not shorted).
  bool lineIdleHigh();

  bool reset() override;
  void writeBit(bool bit) override;
  bool readBit() override;
  void sleepMs(uint32_t ms) override;

 private:
  gpio_num_t pin_;
};
