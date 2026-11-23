// SPDX-License-Identifier: MIT
#include "util/log.h"

#include <stdio.h>

namespace gm {

static void defaultSink(LogLevel level, const char* tag, const char* message) {
  static const char kLevels[] = {'E', 'W', 'I', 'D'};
  fprintf(stderr, "%c [%s] %s\n", kLevels[static_cast<int>(level)], tag,
          message);
}

static LogSink g_sink = defaultSink;

void setLogSink(LogSink sink) { g_sink = sink != nullptr ? sink : defaultSink; }

void logf(LogLevel level, const char* tag, const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_sink(level, tag, buf);
}

}  // namespace gm
