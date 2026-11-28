// SPDX-License-Identifier: MIT
#include "real_sensor.h"

#include "sdkconfig.h"

#ifdef CONFIG_OW_REAL_SENSOR
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "slave_bus.h"
#include "soc/gpio_struct.h"

namespace {
constexpr uint32_t kMask = 1u << CONFIG_OW_MASTER_GPIO;
constexpr uint32_t kCpuMHz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
static_assert(CONFIG_OW_MASTER_GPIO != CONFIG_OW_SLAVE_GPIO,
              "Master/slave GPIOs must differ");

inline void IRAM_ATTR release() { GPIO.out_w1ts.val = kMask; }
inline void IRAM_ATTR pullLow() { GPIO.out_w1tc.val = kMask; }
inline bool IRAM_ATTR high() { return (GPIO.in.val & kMask) != 0; }
inline uint32_t IRAM_ATTR cycles() { return esp_cpu_get_cycle_count(); }

bool IRAM_ATTR waitUs(uint32_t us, uint32_t tolerance = 8) {
  const uint32_t start = cycles();
  while (cycles() - start < us * kCpuMHz) {
  }
  return cycles() - start <= (us + tolerance) * kCpuMHz;
}

bool IRAM_ATTR reset() {
  if (!high()) return false;
  pullLow();
  const bool lowOk = waitUs(490, 100);
  release();
  const bool sampleOk = waitUs(70, 15);
  const bool present = !high();
  waitUs(410, 100);
  return lowOk && sampleOk && present && high();
}

bool IRAM_ATTR writeByte(uint8_t byte) {
  for (unsigned i = 0; i < 8; ++i) {
    const bool bit = (byte >> i) & 1;
    pullLow();
    const bool valid = waitUs(bit ? 6 : 60);
    release();
    waitUs(bit ? 64 : 10);
    if (!valid) return false;
  }
  return true;
}

bool IRAM_ATTR readByte(uint8_t* byte) {
  *byte = 0;
  for (unsigned i = 0; i < 8; ++i) {
    pullLow();
    const uint32_t start = cycles();
    const bool lowOk = waitUs(3, 5);
    release();
    while (cycles() - start < 15 * kCpuMHz) {
    }
    const bool bit = high();
    const bool sampleOk = cycles() - start <= 23 * kCpuMHz;
    waitUs(55);
    if (!lowOk || !sampleOk) return false;
    if (bit) *byte |= 1u << i;
  }
  return true;
}

void sensorTask(void*) {
  while (true) {
    const TickType_t started = xTaskGetTickCount();
    bool ok = reset() && writeByte(0xcc) && writeByte(0x44);
    if (ok) {
      vTaskDelay(pdMS_TO_TICKS(750));
      ok = reset() && writeByte(0xcc) && writeByte(0xbe);
    }
    uint8_t scratch[9] = {};
    for (size_t i = 0; ok && i < 9; ++i) ok = readByte(&scratch[i]);
    release();
    if (ok && ow::crc8(scratch, 8) == scratch[8]) {
      const int16_t temperature =
          static_cast<int16_t>(static_cast<uint16_t>(scratch[0]) |
                               (static_cast<uint16_t>(scratch[1]) << 8));
      if (temperature >= -55 * 16 && temperature <= 125 * 16) {
        slaveBusRealTemperature(temperature);
      } else {
        slaveBusRealFailure();
        ESP_LOGW("real", "temperature out of DS18B20 range");
      }
    } else {
      slaveBusRealFailure();
      ESP_LOGW("real", "read failed: missing presence, preemption or CRC");
    }
    const TickType_t elapsed = xTaskGetTickCount() - started;
    vTaskDelay(elapsed < pdMS_TO_TICKS(1000) ? pdMS_TO_TICKS(1000) - elapsed
                                             : 1);
  }
}
}  // namespace

esp_err_t realSensorStart() {
  if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_OW_MASTER_GPIO))
    return ESP_ERR_INVALID_ARG;
  gpio_config_t config = {};
  config.pin_bit_mask = kMask;
  config.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  esp_err_t err = gpio_config(&config);
  if (err != ESP_OK) return err;
  release();
  return xTaskCreate(sensorTask, "real_ds18b20", 3072, nullptr, 2, nullptr) ==
                 pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}
#else
esp_err_t realSensorStart() { return ESP_OK; }
#endif
