#include "base/log/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace softg::log {

static const char* levelTag(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

void write(Level level, const char* fmt, ...) {
    char buf[1024];
    int prefix = std::snprintf(buf, sizeof(buf), "[SoftG %s] ", levelTag(level));

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf + prefix, sizeof(buf) - prefix - 2, fmt, args);
    va_end(args);

    size_t len = std::strlen(buf);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    std::fputs(buf, stdout);
    OutputDebugStringA(buf);
}

} // namespace softg::log
