#include "util/log.h"

#include <cstdio>

namespace ore {
namespace {

void default_sink(LogLevel level, const char *message) {
	static const char *names[] = {"debug", "info", "warn", "error"};
	fprintf(stderr, "[%s] %s\n", names[int(level)], message);
}

LogSink g_sink = default_sink;
LogLevel g_level = LogLevel::Info;

}  // namespace

void set_log_sink(LogSink sink) {
	g_sink = sink ? sink : default_sink;
}

void set_log_level(LogLevel level) {
	g_level = level;
}

void log_msg(LogLevel level, const char *fmt, ...) {
	if (int(level) < int(g_level)) return;
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	g_sink(level, buf);
}

}  // namespace ore
