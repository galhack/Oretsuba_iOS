// Minimal logging with a replaceable sink.
#pragma once

#include <cstdarg>

namespace ore {

enum class LogLevel { Debug, Info, Warn, Error };

using LogSink = void (*)(LogLevel level, const char *message);
void set_log_sink(LogSink sink);
void set_log_level(LogLevel level);

void log_msg(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define log_debug(...) ::ore::log_msg(::ore::LogLevel::Debug, __VA_ARGS__)
#define log_info(...) ::ore::log_msg(::ore::LogLevel::Info, __VA_ARGS__)
#define log_warn(...) ::ore::log_msg(::ore::LogLevel::Warn, __VA_ARGS__)
#define log_error(...) ::ore::log_msg(::ore::LogLevel::Error, __VA_ARGS__)

}  // namespace ore
