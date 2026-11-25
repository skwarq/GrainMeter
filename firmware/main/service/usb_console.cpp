// SPDX-License-Identifier: MIT
#include "service/usb_console.h"

#include <cstring>

#include "app/shell.h"
#include "device.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile int64_t s_lastLineUs = 0;

// The driver's ring buffer rejects a write larger than its TX buffer as a
// whole (status/diag JSON is several KB), so send in chunks and follow
// partial writes.
static void writeAll(const char* p, size_t n) {
  while (n > 0) {
    const size_t chunk = n > 256 ? 256 : n;
    const int w = usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(100));
    if (w <= 0) return;  // host not reading: drop the rest
    p += w;
    n -= static_cast<size_t>(w);
  }
}

static void writeOut(void*, const char* text) {
  // Convert \n to \r\n for terminals.
  const char* p = text;
  while (*p != '\0') {
    const char* nl = std::strchr(p, '\n');
    const size_t n =
        nl != nullptr ? static_cast<size_t>(nl - p) : std::strlen(p);
    writeAll(p, n);
    if (nl == nullptr) break;
    writeAll("\r\n", 2);
    p = nl + 1;
  }
}

static void consoleTask(void*) {
  static char line[gm::kShellMaxLine];  // console task only
  size_t len = 0;
  writeOut(nullptr, "\nGrainMeter console - type 'help'\n> ");
  while (true) {
    uint8_t c;
    if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(200)) <= 0) {
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (len == 0) {
        writeOut(nullptr, "\n> ");
        continue;
      }
      line[len] = '\0';
      len = 0;
      s_lastLineUs = esp_timer_get_time();
      writeOut(nullptr, "\n");
      bool restart = false;
      {
        DeviceLock lock;
        static gm::Shell* shell =
            new gm::Shell(*device().app, writeOut, nullptr);
        shell->exec(line);
        restart = shell->restartRequested();
      }
      if (restart) {
        device().sys.restart = true;
      }
      writeOut(nullptr, "> ");
    } else if (c == 0x08 || c == 0x7F) {
      if (len > 0) {
        --len;
        writeOut(nullptr, "\b \b");
      }
    } else if (c >= 0x20 && len + 1 < sizeof(line)) {
      line[len++] = static_cast<char>(c);
      const char echo[2] = {static_cast<char>(c), '\0'};
      writeOut(
          nullptr,
          echo);  // note: 'set wifi_pass' echoes while typing, never logged
    }
  }
}

void usbConsoleStart() {
  usb_serial_jtag_driver_config_t cfg = {};
  cfg.rx_buffer_size = 512;
  cfg.tx_buffer_size = 1024;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
    return;
  }
  s_lastLineUs = -60 * 1000000LL;
  xTaskCreate(consoleTask, "console", 6144, nullptr, 3, nullptr);
}

bool usbConnected() { return usb_serial_jtag_is_connected(); }

uint32_t usbConsoleIdleSeconds() {
  return static_cast<uint32_t>((esp_timer_get_time() - s_lastLineUs) / 1000000);
}
