#pragma once

#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <vine/logging/LogLevel.hpp>
#include <vine/logging/LogSink.hpp>

VN_ROOT_NS_BEGIN
namespace logging
{

/**
 * @brief Maps a public LogLevel to the internal spdlog level.
 */
inline spdlog::level::level_enum toSpdlogLevel(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace:    return spdlog::level::trace;
    case LogLevel::Debug:    return spdlog::level::debug;
    case LogLevel::Info:     return spdlog::level::info;
    case LogLevel::Warn:     return spdlog::level::warn;
    case LogLevel::Error:    return spdlog::level::err;
    case LogLevel::Critical: return spdlog::level::critical;
    case LogLevel::Off:      return spdlog::level::off;
    }
    return spdlog::level::info;
}

/**
 * @brief Maps an internal spdlog level to the public LogLevel.
 */
inline LogLevel toLogLevel(spdlog::level::level_enum level) noexcept
{
    switch (level) {
    case spdlog::level::trace:    return LogLevel::Trace;
    case spdlog::level::debug:    return LogLevel::Debug;
    case spdlog::level::info:     return LogLevel::Info;
    case spdlog::level::warn:     return LogLevel::Warn;
    case spdlog::level::err:      return LogLevel::Error;
    case spdlog::level::critical: return LogLevel::Critical;
    case spdlog::level::off:      return LogLevel::Off;
    default:                      return LogLevel::Info;
    }
}

} // namespace logging
VN_ROOT_NS_END

struct vn::logging::LogSink::Impl
{
    spdlog::sink_ptr sink;
};
