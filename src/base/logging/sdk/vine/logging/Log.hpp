#pragma once

#include "logging_global.hpp"

#include <string>
#include <vector>

#include "LogLevel.hpp"
#include "LogSink.hpp"
#include "Logger.hpp"

VN_LOGGING_NS_BEGIN

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
VN_LOGGING_API void initDefault(LogConfig config = {});

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
VN_LOGGING_API Logger& defaultLogger() noexcept;

/**
 * @brief Flushes the default logger.
 */
VN_LOGGING_API void flushDefault();

VN_LOGGING_NS_END

/*
 * Macros logging to the default logger.
 *
 * Each expansion carries VN_HERE (the call site) together with an
 * std::format string and its arguments, for example:
 *
 *     VN_LOGI("mesh loaded, {} triangles", count);
 *
 * The location has to come from the expansion: a member call cannot see its
 * caller, so Logger's level methods take it as a parameter instead of guessing.
 * That is why this entry point is a macro.
 *
 * Levels: VN_LOGT(trace) VN_LOGD(debug) VN_LOGI(info)
 *         VN_LOGW(warn)  VN_LOGE(error) VN_LOGC(critical)
 *
 * They never throw: formatting and sink failures are reported once on stderr and
 * dropped, so macro logging is safe inside catch blocks, noexcept functions and
 * detached coroutines.
 */
#define VN_LOGT(...) ::vn::logging::defaultLogger().trace(VN_HERE, __VA_ARGS__)
#define VN_LOGD(...) ::vn::logging::defaultLogger().debug(VN_HERE, __VA_ARGS__)
#define VN_LOGI(...) ::vn::logging::defaultLogger().info(VN_HERE, __VA_ARGS__)
#define VN_LOGW(...) ::vn::logging::defaultLogger().warn(VN_HERE, __VA_ARGS__)
#define VN_LOGE(...) ::vn::logging::defaultLogger().error(VN_HERE, __VA_ARGS__)
#define VN_LOGC(...) ::vn::logging::defaultLogger().critical(VN_HERE, __VA_ARGS__)
