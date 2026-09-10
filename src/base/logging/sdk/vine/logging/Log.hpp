#pragma once

#include "logging_global.hpp"

#include <string>
#include <vector>

#include "LogLevel.hpp"
#include "LogSink.hpp"
#include "Logger.hpp"

V_LOGGING_NS_BEGIN

/**
 * @brief Configuration for the process-wide default logger.
 *
 * sinks holds the explicit destinations (console, stream, rotating file,
 * custom). When sinks is empty, initDefault adds a colored console sink so
 * the default logger always has at least one sink.
 */
struct LogConfig {
    /// Minimum level of records to log.
    LogLevel level = LogLevel::Info;

    /// Format pattern; empty uses the spdlog default.
    std::string pattern;

    /// Explicit destinations; empty adds a console sink.
    std::vector<LogSink> sinks;
};

/**
 * @brief Reconfigures the process-wide default logger.
 *
 * Rebuilds the logger from config, adding a colored console sink when sinks
 * is empty so the default logger always has at least one sink. This is the one
 * logging entry point that may throw (the sinks are created here); once it
 * returned, logging never throws again.
 *
 * @param config Logger configuration; defaults to console + Info level.
 */
V_LOGGING_API void initDefault(LogConfig config = {});

/**
 * @brief Returns the process-wide default logger.
 *
 * Never throws. The logger is built on first use (so a logger used by another
 * translation unit's static initializer cannot run before this one); when even
 * that fails the silent logger is returned instead, after reporting the failure
 * once on stderr.
 *
 * @return The default logger.
 */
V_LOGGING_API Logger& defaultLogger() noexcept;

/**
 * @brief Flushes the default logger.
 */
V_LOGGING_API void flushDefault();

V_LOGGING_NS_END

/*
 * Macros logging to the default logger.
 *
 * They forward the source location (std::source_location::current()) together
 * with an std::format string and its arguments, for example:
 *
 *     V_LOGI("mesh loaded, {} triangles", count);
 *
 * Levels: V_LOGT(trace) V_LOGD(debug) V_LOGI(info)
 *         V_LOGW(warn)  V_LOGE(error) V_LOGC(critical)
 *
 * They never throw: formatting and sink failures are reported once on stderr and
 * dropped, so macro logging is safe inside catch blocks, noexcept functions and
 * detached coroutines.
 */
#define V_LOGT(...) ::vine::logging::defaultLogger().trace(::std::source_location::current(), __VA_ARGS__)
#define V_LOGD(...) ::vine::logging::defaultLogger().debug(::std::source_location::current(), __VA_ARGS__)
#define V_LOGI(...) ::vine::logging::defaultLogger().info(::std::source_location::current(), __VA_ARGS__)
#define V_LOGW(...) ::vine::logging::defaultLogger().warn(::std::source_location::current(), __VA_ARGS__)
#define V_LOGE(...) ::vine::logging::defaultLogger().error(::std::source_location::current(), __VA_ARGS__)
#define V_LOGC(...) ::vine::logging::defaultLogger().critical(::std::source_location::current(), __VA_ARGS__)
