#pragma once

#include "logging_global.hpp"

#include <format>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "LogLevel.hpp"
#include "LogSink.hpp"

V_LOGGING_NS_BEGIN

/**
 * @brief A named logger dispatching records to a set of sinks.
 *
 * Logger wraps an spdlog logger behind a private implementation, so no spdlog
 * header leaks into this interface. A logger is a cheap shared value: copies
 * refer to the same underlying spdlog logger.
 */
class V_LOGGING_API Logger
{
  public:
    /**
     * @brief Creates an empty logger that produces no output.
     */
    Logger();

    /**
     * @brief Creates a logger writing to a colored console sink.
     *
     * @param name Logger name.
     * @param level Initial level.
     */
    explicit Logger(std::string name, LogLevel level = LogLevel::Info);

    /**
     * @brief Creates a logger with the given sinks and pattern.
     *
     * @param name Logger name.
     * @param level Initial level.
     * @param sinks Destinations; an empty list produces no output.
     * @param pattern Format pattern; an empty string uses the spdlog default.
     */
    Logger(std::string name, LogLevel level, std::vector<LogSink> sinks, std::string pattern = {});

    const std::string& name() const;

    LogLevel level() const;

    /**
     * @brief Sets the minimum level of records passed to sinks.
     *
     * @param level Minimum level.
     */
    void setLevel(LogLevel level);

    /**
     * @brief Sets the spdlog format pattern for this logger.
     *
     * The pattern controls what appears in each line. Common flags:
     *
     *   %n  logger name         %l  level (short)      %L  level (long)
     *   %t  thread id           %P  process id         %v  message payload
     *   %s  source file (base)  %g  source file (full) %#  source line
     *   %@  source location     %!  source function
     *
     * Date/time (chrono style):
     *   %Y %m %d %H %M %S  year / month / day / hour / minute / second
     *   %e  milliseconds     %f  microseconds
     *   %z  timezone offset  %Z  timezone name
     *   %E  epoch seconds
     *
     *   %x  short date          %X  long date/time
     *   %+  spdlog default pattern       %%  literal %
     *
     * Example: "[%Y-%m-%d %H:%M:%S.%e] [%t] [%l] [%n] %v"
     *
     * @param pattern Format pattern; an empty string restores the spdlog default.
     */
    void setPattern(const std::string& pattern);

    /**
     * @brief Attaches an additional sink to this logger.
     *
     * The sink inherits the logger's current format pattern. Adding a sink is
     * not synchronized with concurrent logging; attach sinks before the logger
     * is shared across threads.
     *
     * @param sink Destination to append.
     */
    void addSink(LogSink sink);

    /**
     * @brief Flushes all sinks of this logger.
     */
    void flush();

    /**
     * @brief Returns whether a record at the given level would be written.
     *
     * @param level Log level.
     * @return true if the level is enabled.
     */
    bool isEnabled(LogLevel level) const;

    /**
     * @brief Logs a pre-formatted message at the given level.
     *
     * Never throws: a sink that fails to write is reported once on stderr and the
     * record is dropped, so logging can never decide what the caller does next.
     *
     * @param level Log level.
     * @param message Message text; no formatting is applied.
     * @param loc Source location, defaulting to the call site.
     */
    void log(LogLevel level, std::string message, const std::source_location& loc = std::source_location::current()) noexcept;

    /**
     * @brief Formatted logging entry points, one per level.
     *
     * Each overload formats fmt with args (std::vformat) and writes the record
     * through the configured sinks. None of them throws: a formatting or sink
     * failure is dropped after being reported once on stderr, so a logging call can
     * never change what the caller does next. The overloads taking a
     * std::source_location keep the caller's location; the others use
     * std::source_location::current().
     */
    /// Logs at trace level.
    template <typename... Args>
    void trace(std::string_view fmt, Args&&... args) noexcept;

    /// Logs at debug level.
    template <typename... Args>
    void debug(std::string_view fmt, Args&&... args) noexcept;

    /// Logs at info level.
    template <typename... Args>
    void info(std::string_view fmt, Args&&... args) noexcept;

    /// Logs at warning level.
    template <typename... Args>
    void warn(std::string_view fmt, Args&&... args) noexcept;

    /// Logs at error level.
    template <typename... Args>
    void error(std::string_view fmt, Args&&... args) noexcept;

    /// Logs at critical level.
    template <typename... Args>
    void critical(std::string_view fmt, Args&&... args) noexcept;

    /// Logs at trace level from the given source location.
    template <typename... Args>
    void trace(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /// Logs at debug level from the given source location.
    template <typename... Args>
    void debug(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /// Logs at info level from the given source location.
    template <typename... Args>
    void info(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /// Logs at warning level from the given source location.
    template <typename... Args>
    void warn(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /// Logs at error level from the given source location.
    template <typename... Args>
    void error(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /// Logs at critical level from the given source location.
    template <typename... Args>
    void critical(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

  private:
    /// Formats and writes one record; every failure is swallowed and reported once.
    template <typename... Args>
    void writeFormatted(LogLevel level, const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    struct Impl;
    std::shared_ptr<Impl> d;
};

/**
 * @brief Reports that logging itself failed, once per process.
 *
 * Called from the noexcept logging path when formatting or a sink threw. The
 * notice goes to stderr because the logger is what broke; it is emitted only the
 * first time, so a broken sink cannot flood the console.
 */
V_LOGGING_API void reportLoggingFailure() noexcept;

template <typename... Args>
void Logger::trace(std::string_view fmt, Args&&... args) noexcept
{
    trace(std::source_location::current(), fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::debug(std::string_view fmt, Args&&... args) noexcept
{
    debug(std::source_location::current(), fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::info(std::string_view fmt, Args&&... args) noexcept
{
    info(std::source_location::current(), fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::warn(std::string_view fmt, Args&&... args) noexcept
{
    warn(std::source_location::current(), fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::error(std::string_view fmt, Args&&... args) noexcept
{
    error(std::source_location::current(), fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::critical(std::string_view fmt, Args&&... args) noexcept
{
    critical(std::source_location::current(), fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::trace(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept
{
    writeFormatted(LogLevel::Trace, loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::debug(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept
{
    writeFormatted(LogLevel::Debug, loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::info(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept
{
    writeFormatted(LogLevel::Info, loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::warn(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept
{
    writeFormatted(LogLevel::Warn, loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::error(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept
{
    writeFormatted(LogLevel::Error, loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::critical(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept
{
    writeFormatted(LogLevel::Critical, loc, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::writeFormatted(LogLevel level, const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept
{
    // Everything is guarded: formatting allocates (std::bad_alloc) and reports
    // mismatches as std::format_error, and the sink call can throw as well. A
    // logging failure is reported once and dropped, never propagated.
    try {
        if (!isEnabled(level)) {
            return;
        }

        std::string message;
        try {
            message = std::vformat(fmt, std::make_format_args(args...));
        }
        catch (const std::format_error&) {
            message = std::string(fmt);
        }
        log(level, std::move(message), loc);
    }
    catch (...) {
        reportLoggingFailure();
    }
}

V_LOGGING_NS_END
