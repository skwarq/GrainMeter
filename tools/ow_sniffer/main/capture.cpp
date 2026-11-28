// SPDX-License-Identifier: MIT
#include "capture.h"

#include "driver/gpio.h"
#include "driver/gpio_filter.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace ow {
namespace {
constexpr gpio_num_t kData = static_cast<gpio_num_t>(CONFIG_OW_DATA_GPIO);
constexpr gpio_num_t kLed = GPIO_NUM_15;
RawRecord records[kCapacity];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint64_t written = 0;
size_t write_slot = 0;
uint64_t generation = 0;
uint64_t offset = 0;
uint64_t anomalies = 0;
int64_t last_edge = 0;
int64_t origin = 0;
int64_t last_activity = 0;
uint8_t level = 1;

void IRAM_ATTR edge(void*) {
  const int64_t now = esp_timer_get_time();
  const uint8_t next = static_cast<uint8_t>(gpio_get_level(kData));
  portENTER_CRITICAL_ISR(&mux);
  if (next == level) {
    // A pending interrupt with no state change indicates a missed pair of
    // edges.
    ++anomalies;
  } else {
    const uint64_t duration = static_cast<uint64_t>(now - last_edge);
    const size_t slot = write_slot;
    if (written >= kCapacity) offset += records[slot].duration_us;
    records[slot].duration_us = duration;
    records[slot].level = level;
    ++written;
    if (++write_slot == kCapacity) write_slot = 0;
    last_edge = now;
    last_activity = now;
    level = next;
  }
  portEXIT_CRITICAL_ISR(&mux);
}
}  // namespace

esp_err_t captureStart() {
  gpio_config_t input{};
  input.pin_bit_mask = 1ULL << kData;
  input.mode = GPIO_MODE_INPUT;
#if CONFIG_OW_INTERNAL_PULLUP
  input.pull_up_en = GPIO_PULLUP_ENABLE;  // diode level shifter to a 5 V bus
#else
  input.pull_up_en = GPIO_PULLUP_DISABLE;
#endif
  input.pull_down_en = GPIO_PULLDOWN_DISABLE;
  input.intr_type = GPIO_INTR_DISABLE;
  ESP_RETURN_ON_ERROR(gpio_config(&input), "capture", "DATA input");
  gpio_flex_glitch_filter_config_t filter{};
  filter.clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT;
  filter.gpio_num = kData;
  filter.window_width_ns = 3000;
  filter.window_thres_ns = 3000;
  gpio_glitch_filter_handle_t handle = nullptr;
  ESP_RETURN_ON_ERROR(gpio_new_flex_glitch_filter(&filter, &handle), "capture",
                      "allocate glitch filter");
  ESP_RETURN_ON_ERROR(gpio_glitch_filter_enable(handle), "capture",
                      "enable glitch filter");
  ESP_RETURN_ON_ERROR(
      gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3),
      "capture", "GPIO ISR service");
  ESP_RETURN_ON_ERROR(gpio_isr_handler_add(kData, edge, nullptr), "capture",
                      "GPIO callback");
  captureClear();
  ESP_RETURN_ON_ERROR(gpio_set_intr_type(kData, GPIO_INTR_ANYEDGE), "capture",
                      "both edges");
  ESP_RETURN_ON_ERROR(gpio_intr_enable(kData), "capture", "enable capture");
  gpio_config_t led{};
  led.pin_bit_mask = 1ULL << kLed;
  led.mode = GPIO_MODE_OUTPUT;
  ESP_RETURN_ON_ERROR(gpio_config(&led), "capture", "LED");
  return gpio_set_level(kLed, 0);
}

void captureClear() {
  portENTER_CRITICAL(&mux);
  written = offset = anomalies = 0;
  write_slot = 0;
  ++generation;
  last_edge = esp_timer_get_time();
  origin = last_edge;
  last_activity = 0;
  level = static_cast<uint8_t>(gpio_get_level(kData));
  portEXIT_CRITICAL(&mux);
}

Snapshot captureSnapshot() {
  Snapshot result{};
  portENTER_CRITICAL(&mux);
  const int64_t now = esp_timer_get_time();
  result.end = written;
  result.begin = written > kCapacity ? written - kCapacity : 0;
  result.generation = generation;
  result.offset_us = offset;
  result.overwritten = result.begin;
  result.anomalies = anomalies;
  result.uptime_us = static_cast<uint64_t>(now);
  result.recording_us = static_cast<uint64_t>(now - origin);
  result.tail = {static_cast<uint64_t>(now - last_edge), level};
  portEXIT_CRITICAL(&mux);
  return result;
}

bool captureRead(const Snapshot& snapshot, uint64_t index, RawRecord* record) {
  bool valid;
  portENTER_CRITICAL(&mux);
  valid = snapshot.generation == generation && index >= snapshot.begin &&
          index < snapshot.end && written - index <= kCapacity;
  if (valid) *record = records[index % kCapacity];
  portEXIT_CRITICAL(&mux);
  return valid;
}

bool captureValid(const Snapshot& snapshot) {
  portENTER_CRITICAL(&mux);
  const bool valid = snapshot.generation == generation;
  portEXIT_CRITICAL(&mux);
  return valid;
}

void activityTask(void*) {
  bool on = false;
  while (true) {
    portENTER_CRITICAL(&mux);
    const bool active =
        last_activity != 0 && esp_timer_get_time() - last_activity < 150000;
    portEXIT_CRITICAL(&mux);
    on = active && !on;
    ESP_ERROR_CHECK(gpio_set_level(kLed, on));
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
}  // namespace ow
