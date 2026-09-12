// 简单日志：stdout + OutputDebugString（调试器可见）。
#pragma once

#include <cstdint>

namespace softg::log {

enum class Level : uint8_t { Debug, Info, Warn, Error };

void write(Level level, const char* fmt, ...);

} // namespace softg::log

#define SOFTG_LOG_DEBUG(...) ::softg::log::write(::softg::log::Level::Debug, __VA_ARGS__)
#define SOFTG_LOG_INFO(...)  ::softg::log::write(::softg::log::Level::Info,  __VA_ARGS__)
#define SOFTG_LOG_WARN(...)  ::softg::log::write(::softg::log::Level::Warn,  __VA_ARGS__)
#define SOFTG_LOG_ERROR(...) ::softg::log::write(::softg::log::Level::Error, __VA_ARGS__)
