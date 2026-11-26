// SPDX-License-Identifier: MIT
#include "hw/board_io.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {
esp_timer_handle_t s_blink = nullptr;
bool s_ledState = false;

void blinkCb(void*) {
  s_ledState = !s_ledState;
  ledSet(s_ledState);
}
}  // namespace

void boardIoInit() {
  gpio_config_t btn = {};
  btn.pin_bit_mask = 1ULL << CONFIG_GM_SERVICE_BUTTON_GPIO;
  btn.mode = GPIO_MODE_INPUT;
  btn.pull_up_en = GPIO_PULLUP_ENABLE;  // board has 10k too
  gpio_config(&btn);
#if CONFIG_GM_SERVICE_WAKE_GPIO >= 0
  // deepSleep() hands the pin to the LP IO domain: take it back.
  rtc_gpio_deinit(static_cast<gpio_num_t>(CONFIG_GM_SERVICE_WAKE_GPIO));
  gpio_config_t wake = {};
  wake.pin_bit_mask = 1ULL << CONFIG_GM_SERVICE_WAKE_GPIO;
  wake.mode = GPIO_MODE_INPUT;
  wake.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&wake);
#endif
#if CONFIG_GM_LED_GPIO >= 0
  gpio_config_t led = {};
  led.pin_bit_mask = 1ULL << CONFIG_GM_LED_GPIO;
  led.mode = GPIO_MODE_OUTPUT;
  gpio_config(&led);
  gpio_set_level(static_cast<gpio_num_t>(CONFIG_GM_LED_GPIO), 0);
  esp_timer_create_args_t args = {};
  args.callback = blinkCb;
  args.name = "led";
  esp_timer_create(&args, &s_blink);
#endif
}

void ledSet(bool on) {
#if CONFIG_GM_LED_GPIO >= 0
  gpio_set_level(static_cast<gpio_num_t>(CONFIG_GM_LED_GPIO), on ? 1 : 0);
#else
  (void)on;
#endif
}

void ledBlink(int periodMs) {
  if (s_blink == nullptr) {
    return;
  }
  esp_timer_stop(s_blink);
  if (periodMs > 0) {
    esp_timer_start_periodic(s_blink, static_cast<uint64_t>(periodMs) * 500u);
  } else {
    ledSet(false);
  }
}

bool serviceButtonPressed() {
  const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_GM_SERVICE_BUTTON_GPIO);
  if (gpio_get_level(pin) != 0) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  return gpio_get_level(pin) == 0;
}

bool serviceWakeButtonPressed() {
#if CONFIG_GM_SERVICE_WAKE_GPIO >= 0
  const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_GM_SERVICE_WAKE_GPIO);
  if (gpio_get_level(pin) != 0) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  return gpio_get_level(pin) == 0;
#else
  return false;
#endif
}

bool waitForServiceButton(int windowMs) {
  ledBlink(150);
  const int64_t end =
      esp_timer_get_time() + static_cast<int64_t>(windowMs) * 1000;
  bool pressed = false;
  while (esp_timer_get_time() < end && !pressed) {
    pressed = serviceButtonPressed();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  ledBlink(0);
  return pressed;
}
