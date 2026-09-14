// 简单日志：stdout + OutputDebugString（调试器可见）。
#pragma once

#include <cstdint>

namespace pv::log {

enum class Level : uint8_t { Debug, Info, Warn, Error };

void write(Level level, const char* fmt, ...);

} // namespace pv::log

#define PV_LOG_DEBUG(...) ::pv::log::write(::pv::log::Level::Debug, __VA_ARGS__)
#define PV_LOG_INFO(...)  ::pv::log::write(::pv::log::Level::Info,  __VA_ARGS__)
#define PV_LOG_WARN(...)  ::pv::log::write(::pv::log::Level::Warn,  __VA_ARGS__)
#define PV_LOG_ERROR(...) ::pv::log::write(::pv::log::Level::Error, __VA_ARGS__)
