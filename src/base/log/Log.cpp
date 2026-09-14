#include "base/log/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace pv::log {

static const char* levelTag(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

// 日志文本为 UTF-8。挂接真实控制台时用 WriteConsoleW 按 UTF-16 渲染，
// 不受控制台代码页影响（GBK 控制台直接 fputs UTF-8 会显示乱码）；
// 重定向到文件/管道时保持 UTF-8 原样写入。
static void writeToConsoleOrStdout(const char* text) {
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != nullptr && out != INVALID_HANDLE_VALUE && ::GetConsoleMode(out, &mode)) {
        std::fflush(stdout);  // 与 stdio 缓冲保持输出顺序一致
        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        std::wstring wide(wlen > 0 ? wlen : 1, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), wlen);
        DWORD written = 0;
        ::WriteConsoleW(out, wide.c_str(), (DWORD)wide.size() - 1, &written, nullptr);
    } else {
        std::fputs(text, stdout);
    }
}

void write(Level level, const char* fmt, ...) {
    char buf[1024];
    int prefix = std::snprintf(buf, sizeof(buf), "[PlanView %s] ", levelTag(level));

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf + prefix, sizeof(buf) - prefix - 2, fmt, args);
    va_end(args);

    size_t len = std::strlen(buf);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    writeToConsoleOrStdout(buf);

    // OutputDebugStringA 按 ANSI 代码页解释，UTF-8 同样会乱码，走宽字符版
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, buf, -1, nullptr, 0);
    std::wstring wide(wlen > 0 ? wlen : 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, buf, -1, wide.data(), wlen);
    ::OutputDebugStringW(wide.c_str());
}

} // namespace pv::log
