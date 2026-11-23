// SPDX-License-Identifier: MIT
#include "hw/battery.h"

#include "sdkconfig.h"

#if CONFIG_GM_BATTERY_ADC
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char* kTag = "battery";

int batteryReadMv() {
  adc_unit_t unit;
  adc_channel_t channel;
  if (adc_oneshot_io_to_channel(CONFIG_GM_BATTERY_GPIO, &unit, &channel) !=
      ESP_OK) {
    ESP_LOGW(kTag, "GPIO%d has no ADC channel", CONFIG_GM_BATTERY_GPIO);
    return -1;
  }
  adc_oneshot_unit_handle_t adc = nullptr;
  adc_oneshot_unit_init_cfg_t unitCfg = {};
  unitCfg.unit_id = unit;
  if (adc_oneshot_new_unit(&unitCfg, &adc) != ESP_OK) {
    return -1;
  }
  adc_oneshot_chan_cfg_t chCfg = {};
  chCfg.atten = ADC_ATTEN_DB_12;
  chCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  adc_cali_handle_t cali = nullptr;
  adc_cali_curve_fitting_config_t caliCfg = {};
  caliCfg.unit_id = unit;
  caliCfg.chan = channel;
  caliCfg.atten = ADC_ATTEN_DB_12;
  caliCfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  int result = -1;
  if (adc_oneshot_config_channel(adc, channel, &chCfg) == ESP_OK &&
      adc_cali_create_scheme_curve_fitting(&caliCfg, &cali) == ESP_OK) {
    // 1 MOhm source impedance: the 100 nF cap feeds the sampling
    // capacitor; discard the first conversion and average the rest.
    int raw = 0;
    adc_oneshot_read(adc, channel, &raw);
    int64_t sumMv = 0;
    int n = 0;
    for (int i = 0; i < 16; ++i) {
      int mv = 0;
      if (adc_oneshot_read(adc, channel, &raw) == ESP_OK &&
          adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
        sumMv += mv;
        ++n;
      }
    }
    if (n > 0) {
      result =
          static_cast<int>(sumMv / n * CONFIG_GM_BATTERY_DIVIDER_X1000 / 1000);
    }
  } else {
    ESP_LOGW(kTag, "ADC calibration not available");
  }
  if (cali != nullptr) {
    adc_cali_delete_scheme_curve_fitting(cali);
  }
  adc_oneshot_del_unit(adc);
  return result;
}
#else
int batteryReadMv() { return -1; }
#endif
