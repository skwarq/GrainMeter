// SPDX-License-Identifier: MIT
#include "slave_bus.h"

#include <atomic>

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "soc/gpio_struct.h"

namespace {
constexpr uint32_t kMask = 1u << CONFIG_OW_SLAVE_GPIO;
constexpr uint32_t kCyclesPerUs = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
constexpr uint32_t kIdleCycles = 2000 * kCyclesPerUs;
// STW-100 sends its boot frames (2x Search ROM per pass, ~27.6 ms each) with
// only 0.15..1 ms between them, so one polling session spans ~140 ms. Limit
// each reset-delimited frame, and the whole session as a stuck-bus guard.
constexpr uint32_t kFrameLimitCycles = 40000 * kCyclesPerUs;
constexpr uint32_t kSessionLimitCycles = 400000 * kCyclesPerUs;
static_assert(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ == 160,
              "Timing requires fixed 160 MHz CPU");
#ifdef CONFIG_PM_ENABLE
#error "Disable dynamic frequency scaling for the polling slave"
#endif

ow::Slave s_slave;
ow::Config s_config;
ow::Config s_pending;
std::atomic<bool> s_pendingReady{false};
std::atomic<int32_t> s_realValue{0};
std::atomic<bool> s_realValid{false};
std::atomic<uint32_t> s_realErrors{0};
BusStats s_timing;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

inline bool IRAM_ATTR high() { return (GPIO.in.val & kMask) != 0; }
inline void IRAM_ATTR release() { GPIO.out_w1ts.val = kMask; }
inline void IRAM_ATTR pullLow() { GPIO.out_w1tc.val = kMask; }
inline uint32_t IRAM_ATTR cycles() { return esp_cpu_get_cycle_count(); }

void IRAM_ATTR applyPending() {
  if (s_pendingReady.load(std::memory_order_acquire)) {
    s_config = s_pending;
    s_slave.configure(s_config);
    s_pendingReady.store(false, std::memory_order_release);
  }
  if (s_realValid.load(std::memory_order_acquire)) {
    const int16_t temperature = s_realValue.load(std::memory_order_relaxed);
    for (size_t i = 0; i < s_config.count; ++i)
      if (s_config.devices[i].realSource)
        s_slave.setTemperature(i, temperature);
  }
}

void IRAM_ATTR recordReaction(uint32_t elapsed) {
  if (elapsed > s_timing.maxReactionCycles)
    s_timing.maxReactionCycles = elapsed;
  if (elapsed >= 8 * kCyclesPerUs) ++s_timing.lateReactions;
}

// Every reachable function/data in this handler is in IRAM/internal DRAM.
void IRAM_ATTR busInterrupt(void*) {
  GPIO.status_w1tc.val = kMask;
  portENTER_CRITICAL_ISR(&s_lock);
  if (high()) {
    portEXIT_CRITICAL_ISR(&s_lock);
    return;
  }
  ++s_timing.firstEdges;
  const uint32_t origin = cycles();
  const uint64_t originUs = esp_timer_get_time();
  uint32_t falling = origin;
  uint32_t lastHigh = origin;
  uint32_t highSince = origin;
  uint32_t frameStart = origin;
  bool low = true;
  bool driving = s_slave.lowStarted(originUs);
  if (driving) {
    pullLow();
    // First-edge ISR latency is NOT observable with software timestamps.
    recordReaction(cycles() - falling);
  }
  bool preparedReset = false;
  bool nextPull = false;
  while (true) {
    const uint32_t now = cycles();
    const uint32_t elapsed = now - origin;
    if (elapsed > kSessionLimitCycles || now - frameStart > kFrameLimitCycles) {
      ++s_timing.overruns;
      if (!high()) ++s_timing.stuckLow;
      break;
    }
    if (low) {
      const uint32_t widthCycles = now - falling;
      if (driving && widthCycles >= ow::kSampleUs * kCyclesPerUs) {
        release();
        driving = false;
      }
      if (!preparedReset && widthCycles >= ow::kResetUs * kCyclesPerUs) {
        // Use the long reset-low interval, not the presence/read deadline.
        applyPending();
        preparedReset = true;
        frameStart = falling;  // a reset starts a new frame
      }
      if (!high()) continue;
      const uint32_t rising = cycles();
      const uint64_t timeUs = originUs + (rising - origin) / kCyclesPerUs;
      const bool presence =
          s_slave.lowEnded((rising - falling) / kCyclesPerUs, timeUs);
      if (presence) {
        while (cycles() - rising < 30 * kCyclesPerUs) {
        }
        pullLow();
        const uint32_t presenceStart = cycles();
        if (presenceStart - rising > 60 * kCyclesPerUs)
          ++s_timing.lateReactions;
        while (cycles() - presenceStart < 117 * kCyclesPerUs) {
        }
        release();
      }
      low = false;
      preparedReset = false;
      lastHigh = presence ? cycles() : rising;
      highSince = lastHigh;
      nextPull =
          !s_slave.bitToSend(originUs + (lastHigh - origin) / kCyclesPerUs);
    } else {
      if (!high()) {
        // Drive first; decode/bookkeeping can wait until after this deadline.
        falling = cycles();
        driving = nextPull;
        if (driving) {
          pullLow();
          // Conservative upper bound: last observed high -> actual pull.
          recordReaction(cycles() - lastHigh);
        }
        s_slave.lowStarted(originUs + (falling - origin) / kCyclesPerUs);
        low = true;
      } else {
        if (now - highSince > kIdleCycles) break;
        lastHigh = cycles();
        // Conversion completion can change a status bit during the high gap.
        nextPull = !s_slave.bitToSend(originUs + elapsed / kCyclesPerUs);
      }
    }
  }
  release();
  s_slave.idle();
  GPIO.status_w1tc.val = kMask;
  portEXIT_CRITICAL_ISR(&s_lock);
}
}  // namespace

esp_err_t slaveBusStart(const ow::Config& config) {
  if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_OW_SLAVE_GPIO))
    return ESP_ERR_INVALID_ARG;
  s_config = config;
  s_slave.configure(config);
  gpio_config_t gpio = {};
  gpio.pin_bit_mask = kMask;
  gpio.mode = GPIO_MODE_INPUT_OUTPUT_OD;
#if CONFIG_OW_INTERNAL_PULLUP
  gpio.pull_up_en = GPIO_PULLUP_ENABLE;  // diode level shifter to a 5 V bus
#else
  gpio.pull_up_en = GPIO_PULLUP_DISABLE;
#endif
  gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio.intr_type = GPIO_INTR_DISABLE;
  esp_err_t err = gpio_config(&gpio);
  if (err != ESP_OK) return err;
  release();
  err = gpio_isr_register(busInterrupt, nullptr,
                          ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3, nullptr);
  if (err != ESP_OK) return err;
  err = gpio_set_intr_type(static_cast<gpio_num_t>(CONFIG_OW_SLAVE_GPIO),
                           GPIO_INTR_NEGEDGE);
  if (err != ESP_OK) return err;
  return gpio_intr_enable(static_cast<gpio_num_t>(CONFIG_OW_SLAVE_GPIO));
}

void slaveBusQueueConfig(const ow::Config& config) {
  // httpd serializes POSTs. Never overwrite a mailbox the ISR is consuming.
  while (s_pendingReady.load(std::memory_order_acquire)) {
    // A new configuration must not wait forever when STW is disconnected.
    portENTER_CRITICAL(&s_lock);
    s_pendingReady.store(false, std::memory_order_release);
    portEXIT_CRITICAL(&s_lock);
  }
  s_pending = config;
  s_pendingReady.store(true, std::memory_order_release);
}

void slaveBusRealTemperature(int16_t temperature) {
  s_realValue.store(temperature, std::memory_order_relaxed);
  s_realValid.store(true, std::memory_order_release);
}

void slaveBusRealFailure() {
  s_realValid.store(false, std::memory_order_release);
  s_realErrors.fetch_add(1, std::memory_order_relaxed);
}

bool slaveBusRealReading(int16_t* temperature) {
  if (!s_realValid.load(std::memory_order_acquire)) return false;
  *temperature = s_realValue.load(std::memory_order_relaxed);
  return true;
}

BusStats slaveBusStats() {
  portENTER_CRITICAL(&s_lock);
  BusStats stats = s_timing;
  stats.protocol = s_slave.counters();
  stats.logSequence = s_slave.logSequence();
  stats.pendingConfig = s_pendingReady.load(std::memory_order_acquire);
  stats.realErrors = s_realErrors.load(std::memory_order_relaxed);
  portEXIT_CRITICAL(&s_lock);
  return stats;
}

size_t slaveBusLogs(ow::Transaction* out, size_t capacity) {
  // Small per-record critical sections, never one 200-record masked copy.
  size_t count;
  uint32_t sequence;
  portENTER_CRITICAL(&s_lock);
  count = s_slave.logCount();
  sequence = s_slave.logSequence();
  portEXIT_CRITICAL(&s_lock);
  if (count > capacity) count = capacity;
  size_t copied = 0;
  while (copied < count) {
    portENTER_CRITICAL(&s_lock);
    const bool changed = sequence != s_slave.logSequence();
    if (!changed) out[copied] = s_slave.logAt(copied);
    portEXIT_CRITICAL(&s_lock);
    if (changed) break;  // return a consistent prefix, not mixed ring entries
    ++copied;
  }
  return copied;
}
