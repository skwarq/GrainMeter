// SPDX-License-Identifier: MIT
#include "hw/onewire_gpio.h"

#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// Standard-speed timings (Maxim AN126, recommended values, microseconds).
namespace {
constexpr uint32_t kResetLowUs = 480;
constexpr uint32_t kPresenceSampleUs = 70;
constexpr uint32_t kResetRecoveryUs = 410;
constexpr uint32_t kWrite1LowUs = 6;
constexpr uint32_t kWrite1HighUs = 64;
constexpr uint32_t kWrite0LowUs = 60;
constexpr uint32_t kWrite0HighUs = 10;
constexpr uint32_t kReadLowUs = 6;
constexpr uint32_t kReadSampleUs = 9;
constexpr uint32_t kReadRecoveryUs = 55;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

esp_err_t OneWireGpio::init() {
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << pin_;
  io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  io.pull_up_en = CONFIG_GM_ONEWIRE_INTERNAL_PULLUP ? GPIO_PULLUP_ENABLE
                                                    : GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  esp_err_t err = gpio_config(&io);
  if (err == ESP_OK) {
    err = gpio_set_level(pin_, 1);  // release the line
  }
  return err;
}

bool OneWireGpio::lineIdleHigh() {
  gpio_set_level(pin_, 1);
  esp_rom_delay_us(5);
  return gpio_get_level(pin_) == 1;
}

bool OneWireGpio::reset() {
  gpio_set_level(pin_, 1);
  gpio_set_level(pin_, 0);
  esp_rom_delay_us(kResetLowUs);
  bool presence;
  taskENTER_CRITICAL(&s_mux);
  gpio_set_level(pin_, 1);
  esp_rom_delay_us(kPresenceSampleUs);
  presence = gpio_get_level(pin_) == 0;
  taskEXIT_CRITICAL(&s_mux);
  esp_rom_delay_us(kResetRecoveryUs);
  return presence;
}

void OneWireGpio::writeBit(bool bit) {
  taskENTER_CRITICAL(&s_mux);
  gpio_set_level(pin_, 0);
  esp_rom_delay_us(bit ? kWrite1LowUs : kWrite0LowUs);
  gpio_set_level(pin_, 1);
  taskEXIT_CRITICAL(&s_mux);
  esp_rom_delay_us(bit ? kWrite1HighUs : kWrite0HighUs);
}

bool OneWireGpio::readBit() {
  bool value;
  taskENTER_CRITICAL(&s_mux);
  gpio_set_level(pin_, 0);
  esp_rom_delay_us(kReadLowUs);
  gpio_set_level(pin_, 1);
  esp_rom_delay_us(kReadSampleUs);
  value = gpio_get_level(pin_) != 0;
  taskEXIT_CRITICAL(&s_mux);
  esp_rom_delay_us(kReadRecoveryUs);
  return value;
}

void OneWireGpio::sleepMs(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1));
}
