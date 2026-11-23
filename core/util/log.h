// SPDX-License-Identifier: MIT
#ifndef GM_LOG_H
#define GM_LOG_H

#include <stdarg.h>

namespace gm {

enum class LogLevel { Error, Warn, Info, Debug };

typedef void (*LogSink)(LogLevel level, const char* tag, const char* message);

// Platform installs a sink (ESP_LOGx on ESP32, stderr on PC). Messages are
// formatted into a bounded buffer and truncated if too long. Secrets must
// never be passed to the logger.
void setLogSink(LogSink sink);
void logf(LogLevel level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

}  // namespace gm

#define GM_LOGE(tag, ...) ::gm::logf(::gm::LogLevel::Error, tag, __VA_ARGS__)
#define GM_LOGW(tag, ...) ::gm::logf(::gm::LogLevel::Warn, tag, __VA_ARGS__)
#define GM_LOGI(tag, ...) ::gm::logf(::gm::LogLevel::Info, tag, __VA_ARGS__)
#define GM_LOGD(tag, ...) ::gm::logf(::gm::LogLevel::Debug, tag, __VA_ARGS__)

#endif
