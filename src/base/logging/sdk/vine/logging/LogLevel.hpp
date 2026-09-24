#pragma once

#include "logging_global.hpp"

#include <string_view>

VN_LOGGING_NS_BEGIN

/**
 * @brief Severity levels of log records.
 *
 * Levels are ordered from most verbose to most severe, followed by Off which
 * disables all output. They map one-to-one onto the underlying spdlog levels.
 */
enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

/**
 * @brief Returns the lowercase name of a log level.
 *
 * @param level Log level.
 * @return The level name.
 */
VN_LOGGING_API std::string_view levelName(LogLevel level) noexcept;

/**
 * @brief Parses a log level name, case-insensitively.
 *
 * Unknown names map to LogLevel::Info.
 *
 * @param name Level name.
 * @return The parsed level.
 */
VN_LOGGING_API LogLevel parseLevel(std::string_view name) noexcept;

VN_LOGGING_NS_END
