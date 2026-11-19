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

VN_LOGGING_NS_BEGIN

/**
 * @brief A named logger dispatching records to a set of sinks.
 *
 * @details
 * - Logger wraps an spdlog logger behind a private implementation, so no spdlog header leaks into this interface.
 * - A logger is a cheap shared value: copies refer to the same underlying spdlog logger.
 * - The formatted level methods take the source location as a parameter because a member function cannot see its
 *   caller: a std::source_location::current() evaluated inside Logger would name this header. The macros in Log.hpp
 *   expand VN_HERE at the call site; a direct call passes the location it means to report.
 */
class VN_LOGGING_API Logger {
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

    /**
     * @brief Returns the logger name.
     *
     * @return The name given at construction; empty for the silent logger.
     */
    const std::string& name() const;

    /**
     * @brief Returns the minimum level of records passed to sinks.
     *
     * @return The current level; LogLevel::Off for the silent logger.
     */
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
     * @brief Logs a formatted record at trace level.
     *
     * @param loc Call site to record; VN_HERE means "here".
     * @param fmt std::format pattern; an argument mismatch writes the pattern itself.
     * @param args Arguments substituted into fmt.
     */
    template <typename... Args>
    void trace(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /**
     * @brief Logs a formatted record at debug level.
     *
     * @param loc Call site to record; VN_HERE means "here".
     * @param fmt std::format pattern; an argument mismatch writes the pattern itself.
     * @param args Arguments substituted into fmt.
     */
    template <typename... Args>
    void debug(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /**
     * @brief Logs a formatted record at info level.
     *
     * @param loc Call site to record; VN_HERE means "here".
     * @param fmt std::format pattern; an argument mismatch writes the pattern itself.
     * @param args Arguments substituted into fmt.
     */
    template <typename... Args>
    void info(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /**
     * @brief Logs a formatted record at warning level.
     *
     * @param loc Call site to record; VN_HERE means "here".
     * @param fmt std::format pattern; an argument mismatch writes the pattern itself.
     * @param args Arguments substituted into fmt.
     */
    template <typename... Args>
    void warn(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /**
     * @brief Logs a formatted record at error level.
     *
     * @param loc Call site to record; VN_HERE means "here".
     * @param fmt std::format pattern; an argument mismatch writes the pattern itself.
     * @param args Arguments substituted into fmt.
     */
    template <typename... Args>
    void error(const std::source_location& loc, std::string_view fmt, Args&&... args) noexcept;

    /**
     * @brief Logs a formatted record at critical level.
     *
     * @param loc Call site to record; VN_HERE means "here".
     * @param fmt std::format pattern; an argument mismatch writes the pattern itself.
     * @param args Arguments substituted into fmt.
     */
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
VN_LOGGING_API void reportLoggingFailure() noexcept;

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

VN_LOGGING_NS_END

/**
 * @brief The current call site, for the level methods' explicit location parameter.
 *
 * @details
 * - A macro rather than a function: std::source_location::current() reports where it is evaluated, so it has to be
 *   evaluated at the call site; a function wrapping it would report its own body.
 * - The VN_LOGx macros use it, and a caller that logs through a Logger it holds writes the same thing:
 *   `logger.info(VN_HERE, "mesh loaded, {} triangles", count)`.
 */
#define VN_HERE ::std::source_location::current()
