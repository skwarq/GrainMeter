// SPDX-License-Identifier: MIT
#ifndef GM_SHELL_H
#define GM_SHELL_H

#include <stddef.h>

#include "app/grainmeter.h"

namespace gm {

// Line-oriented service console: USB Serial/JTAG on the ESP32, stdin on PC.
// Physical access is the trust anchor, so no password is asked here. Secret
// values are accepted but never echoed.
class Shell {
 public:
  typedef void (*Writer)(void* ctx, const char* text);

  Shell(GrainMeter& app, Writer out, void* ctx)
      : app_(app), out_(out), ctx_(ctx), restart_(false) {}

  // Executes one command line. Returns false for an unknown command.
  bool exec(const char* line);
  // Set when the command requested a restart (performed by the platform).
  bool restartRequested() const { return restart_; }

 private:
  void print(const char* text);
  void printf(const char* fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 2, 3)))
#endif
      ;
  void printJson(bool ok);
  bool platform(const char* action, const char* args);

  GrainMeter& app_;
  Writer out_;
  void* ctx_;
  bool restart_;
  char buf_[16384];
};

}  // namespace gm

#endif
